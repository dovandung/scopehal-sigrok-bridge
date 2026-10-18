// Device discovery and configuration on top of the libsigrok4DSL ds_* API
// (DSView >= 1.3 / ALL-LOGIC). All calls act on the activated device.

#include "server.h"
#include "srbinding.h"
#include "log/log.h"

#include <algorithm>
#include <cctype>
#include <math.h>
#include <string.h>

std::string g_devName;
std::string g_driverName;
bool g_deviceIsScope = false;
std::vector<struct sr_channel*> g_channels;

uint64_t g_rate = 0, g_depth = 0, g_trigfs = 0;
uint64_t g_maxDepth = 100000000;
uint8_t g_trigpct = 0;
int g_selectedTriggerChannel = 0;
int g_selectedTriggerDirection = RISING;
std::vector<uint64_t> vdiv_options;
int g_numdivs = DS_CONF_DSO_VDIVS;
uint32_t g_hwmin = 0, g_hwmax = 255;
float g_hwrange_factor = 1;

std::recursive_mutex g_devMutex;
int g_requestedMode = -1;

static std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
	return s;
}

static bool is_number(const std::string& s) {
	return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c){ return std::isdigit(c); });
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Discovery

static std::vector<ds_device_base_info> get_devices() {
	std::vector<ds_device_base_info> out;
	ds_device_base_info* list = NULL;
	int count = 0;
	if (ds_get_device_list(&list, &count) != SR_OK || list == NULL)
		return out;
	for (int i = 0; i < count; i++)
		out.push_back(list[i]);
	g_free(list);
	return out;
}

static bool activate(const ds_device_base_info& dev, ds_device_full_info& info) {
	if (ds_active_device(dev.handle) != SR_OK)
		return false;
	if (ds_get_actived_device_info(&info) != SR_OK)
		return false;
	// ds_active_device() falls back to another device when opening fails
	return info.handle == dev.handle;
}

int list_devices() {
	auto devs = get_devices();
	if (devs.empty()) {
		printf("No devices found.\n");
		return 1;
	}
	printf("Index  Driver               Name\n");
	for (size_t i = 0; i < devs.size(); i++) {
		ds_device_full_info info;
		std::string drv = "?";
		if (activate(devs[i], info))
			drv = info.driver_name;
		printf("%-6zu %-20s %s\n", i, drv.c_str(), devs[i].name);
	}
	return 0;
}

int init_and_find_device(const std::string& selector) {
	auto devs = get_devices();
	if (devs.empty()) {
		LogError("libsigrok4DSL found no devices\n");
		return 1;
	}

	// Match by index, then device name substring, then driver name
	ds_device_full_info info;
	bool found = false;
	std::string want = lower(selector);

	if (is_number(selector)) {
		size_t idx = std::stoul(selector);
		if (idx < devs.size())
			found = activate(devs[idx], info);
	}
	for (size_t i = 0; !found && i < devs.size(); i++) {
		if (lower(devs[i].name).find(want) != std::string::npos)
			found = activate(devs[i], info);
	}
	for (size_t i = 0; !found && i < devs.size(); i++) {
		if (activate(devs[i], info) && lower(info.driver_name) == want)
			found = true;
	}

	if (!found) {
		LogError("No device matches \"%s\". Use --list to see what is connected.\n", selector.c_str());
		return 1;
	}

	g_devName = info.name;
	g_driverName = info.driver_name;
	LogNotice("Using device \"%s\" (driver %s)\n", g_devName.c_str(), g_driverName.c_str());

	int status = 0;
	for (int i = 0; i < 200 && ds_get_actived_device_init_status(&status) == SR_OK && status == SR_ST_INITIALIZING; i++)
		g_usleep(10000);

	if (g_requestedMode >= 0) {
		GVariant* v = g_variant_ref_sink(g_variant_new_int16(g_requestedMode));
		if (ds_set_actived_device_config(NULL, NULL, SR_CONF_DEVICE_MODE, v) != SR_OK)
			LogWarning("Device refused mode %d\n", g_requestedMode);
		g_variant_unref(v);
	}

	int mode = ds_get_actived_device_mode();
	if (mode == LOGIC) {
		g_deviceIsScope = false;
	} else if (mode == DSO) {
		g_deviceIsScope = true;
	} else {
		LogError("Device mode %d is not supported (only logic analyzer and DSO)\n", mode);
		return 1;
	}

	for (GSList* l = ds_get_actived_device_channels(); l; l = l->next)
		g_channels.push_back((struct sr_channel*)l->data);
	LogDebug("Device has %zu channels, %s mode\n", g_channels.size(), g_deviceIsScope ? "DSO" : "logic");

	auto hwdepth = get_dev_config<uint64_t>(SR_CONF_HW_DEPTH);
	if (hwdepth && *hwdepth > 0 && *hwdepth < g_maxDepth)
		g_maxDepth = *hwdepth;
	LogDebug("Max sample depth offered: %lu\n", (unsigned long)g_maxDepth);

	if (g_deviceIsScope) {
		vdiv_options = get_dev_config_options<uint64_t>(SR_CONF_PROBE_VDIV);
		g_hwmin = get_dev_config<uint32_t>(SR_CONF_REF_MIN).value_or(0);
		g_hwmax = get_dev_config<uint32_t>(SR_CONF_REF_MAX).value_or(255);
		if (g_hwmax <= g_hwmin) { g_hwmin = 0; g_hwmax = 255; }
		g_hwrange_factor = 255.f / (g_hwmax - g_hwmin);
	} else {
		// DSLogic: buffer mode gives a pre-trigger buffer. Others ignore this key.
		if (g_driverName.rfind("DSLogic", 0) == 0)
			set_dev_config<std::string>(SR_CONF_OPERATION_MODE, "Buffer Mode");
		ds_trigger_set_mode(SIMPLE_TRIGGER);
	}


	auto rates = get_dev_config_options<uint64_t>(SR_CONF_SAMPLERATE);
	uint64_t rate = get_dev_config<uint64_t>(SR_CONF_SAMPLERATE).value_or(rates.empty() ? 1000000 : rates.front());

	set_trigger_channel(0);
	set_rate(rate);
	set_depth(std::min<uint64_t>(10000, g_maxDepth));

	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Channels

int count_enabled_channels() {
	int n = 0;
	for (auto ch : g_channels)
		if (ch->enabled) n++;
	return n;
}

std::vector<size_t> enabled_channel_indexes() {
	std::vector<size_t> out;
	for (size_t i = 0; i < g_channels.size(); i++)
		if (g_channels[i]->enabled) out.push_back(i);
	return out;
}

bool set_channel_enabled(size_t ch, bool enabled) {
	if (ch >= g_channels.size())
		return false;
	if (g_channels[ch]->enabled == (gboolean)enabled)
		return true;
	if (!enabled && count_enabled_channels() <= 1) {
		LogWarning("Not disabling ch%zu: it is the last enabled channel\n", ch);
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(g_devMutex);
	abort_running_capture();

	bool ok;
	if (g_deviceIsScope)
		ok = set_probe_config<bool>(g_channels[ch], SR_CONF_PROBE_EN, enabled);
	else
		ok = ds_enable_device_channel(g_channels[ch], enabled) == SR_OK;

	// Fewer/more channels can change the reachable sample rate; re-apply.
	set_rate(g_rate);
	set_depth(g_depth);
	if (!g_deviceIsScope)
		set_trigger_channel(g_selectedTriggerChannel);
	return ok;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Trigger

static void update_logic_trigger() {
	bool any = false;
	for (size_t i = 0; i < g_channels.size(); i++) {
		char dir = 'X';
		if ((int)i == g_selectedTriggerChannel && g_channels[i]->enabled) {
			if (g_selectedTriggerDirection == RISING)       dir = 'R';
			else if (g_selectedTriggerDirection == FALLING) dir = 'F';
			else if (g_selectedTriggerDirection == ANY)     dir = 'C';
		}
		if (dir != 'X') any = true;
		ds_trigger_probe_set(g_channels[i]->index, dir, 'X');
	}
	ds_trigger_set_en(any ? 1 : 0);
	ds_trigger_set_pos(g_trigpct);
}

void set_trigger_channel(int ch) {
	std::lock_guard<std::recursive_mutex> lock(g_devMutex);
	g_selectedTriggerChannel = ch;
	if (g_deviceIsScope)
		set_dev_config<uint8_t>(SR_CONF_TRIGGER_SOURCE, ch == 0 ? DSO_TRIGGER_CH0 : DSO_TRIGGER_CH1);
	else
		update_logic_trigger();
}

void set_trigger_direction(int dir) {
	std::lock_guard<std::recursive_mutex> lock(g_devMutex);
	g_selectedTriggerDirection = dir;
	if (g_deviceIsScope) {
		if (dir == RISING)
			set_dev_config<uint8_t>(SR_CONF_TRIGGER_SLOPE, DSO_TRIGGER_RISING);
		else if (dir == FALLING)
			set_dev_config<uint8_t>(SR_CONF_TRIGGER_SLOPE, DSO_TRIGGER_FALLING);
	} else {
		update_logic_trigger();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Timebase

void set_rate(uint64_t rate) {
	std::lock_guard<std::recursive_mutex> lock(g_devMutex);
	set_dev_config<uint64_t>(SR_CONF_SAMPLERATE, rate);
	// Drivers clamp to what the current channel count allows
	g_rate = get_dev_config<uint64_t>(SR_CONF_SAMPLERATE).value_or(rate);
	set_trigfs(g_trigfs);
}

void set_depth(uint64_t depth) {
	std::lock_guard<std::recursive_mutex> lock(g_devMutex);
	if (depth > g_maxDepth) depth = g_maxDepth;
	set_dev_config<uint64_t>(SR_CONF_LIMIT_SAMPLES, depth);
	g_depth = get_dev_config<uint64_t>(SR_CONF_LIMIT_SAMPLES).value_or(depth);
	set_trigfs(g_trigfs);
}

void set_trigfs(uint64_t fs) {
	std::lock_guard<std::recursive_mutex> lock(g_devMutex);

	uint64_t rate = g_rate ? g_rate : 1;
	if (g_deviceIsScope && rate == 1000000000 && count_enabled_channels() == 2)
		rate /= 2;	// DSCope reports 1GS/s per channel when it is 1GS/s total

	double fs_per_sample = 1e15 / rate;
	double fs_in_capture = fs_per_sample * g_depth;
	double pct = fs_in_capture > 0 ? (double)fs / fs_in_capture * 100 : 0;
	if (pct < 0 || pct > 100)
		pct = 0;

	g_trigpct = (uint8_t)pct;
	g_trigfs = (uint64_t)(g_trigpct * fs_in_capture / 100);

	if (g_deviceIsScope)
		set_dev_config<uint8_t>(SR_CONF_HORIZ_TRIGGERPOS, g_trigpct);
	else
		ds_trigger_set_pos(g_trigpct);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// DSO scaling (DSCope)

void compute_scale_and_offset(struct sr_channel* ch, float& scale, float& offset) {
	float vdiv_mV = get_probe_config<uint64_t>(ch, SR_CONF_PROBE_VDIV).value_or(ch->vdiv);

	float full_throw_V = vdiv_mV / 1000 * g_numdivs;
	scale = -1 * g_hwrange_factor / 255.f * full_throw_V;	// ADC codes are upside down
	offset = 128 * scale;					// zero is code 128
}

void shutdown_device() {
	std::lock_guard<std::recursive_mutex> lock(g_devMutex);
	if (ds_is_collecting())
		ds_stop_collect();
	ds_release_actived_device();
	ds_lib_exit();
}
