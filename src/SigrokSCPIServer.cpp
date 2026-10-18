#include "server.h"
#include "SigrokSCPIServer.h"
#include "srbinding.h"

#include <unistd.h>
#include <string.h>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

SigrokSCPIServer::SigrokSCPIServer(ZSOCKET sock)
	: BridgeSCPIServer(sock)
{
}

SigrokSCPIServer::~SigrokSCPIServer()
{
	LogVerbose("Client disconnected\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Identification and capabilities

string SigrokSCPIServer::GetMake()
{
	return "libsigrok4DSL bridge";
}

string SigrokSCPIServer::GetModel()
{
	return g_devName;
}

string SigrokSCPIServer::GetSerial()
{
	char namebuf[64];
	gethostname(namebuf, sizeof(namebuf));
	namebuf[sizeof(namebuf)-1] = 0;
	return g_driverName + "@" + namebuf;
}

string SigrokSCPIServer::GetFirmwareVersion()
{
	return sr_get_lib_version_string();
}

// CHANS? keeps its historical meaning for this bridge (total channel count).
// ANACHANS? / DIGCHANS? give the split for clients that need it.
size_t SigrokSCPIServer::GetAnalogChannelCount()
{
	return g_channels.size();
}

vector<size_t> SigrokSCPIServer::GetSampleRates()
{
	auto rates = get_dev_config_options<uint64_t>(SR_CONF_SAMPLERATE);
	return vector<size_t>(rates.begin(), rates.end());
}

vector<size_t> SigrokSCPIServer::GetSampleDepths()
{
	// Drivers do not list legal depths; offer a 1-2-5 series up to the
	// hardware (or configured) limit. Drivers round to their own granularity.
	vector<size_t> result;
	for (size_t d = 1000; d <= g_maxDepth; d *= 10) {
		for (size_t m : {1, 2, 5}) {
			if (d * m <= g_maxDepth)
				result.push_back(d * m);
		}
	}
	if (result.empty() || result.back() != g_maxDepth)
		result.push_back(g_maxDepth);
	return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Command parsing

bool SigrokSCPIServer::GetChannelID(const std::string& subject, size_t& id_out)
{
	// Channels are named by index: "0", "1", ... "31"
	if (subject.empty() || subject.size() > 3)
		return false;
	for (char c : subject)
		if (c < '0' || c > '9')
			return false;

	size_t id = stoul(subject);
	if (id >= g_channels.size())
		return false;

	id_out = id;
	return true;
}

BridgeSCPIServer::ChannelType SigrokSCPIServer::GetChannelType(size_t /*channel*/)
{
	return g_deviceIsScope ? CH_ANALOG : CH_DIGITAL;
}

bool SigrokSCPIServer::OnQuery(
		const string& line,
		const string& subject,
		const string& cmd)
{
	if (BridgeSCPIServer::OnQuery(line, subject, cmd))
		return true;

	if (cmd == "ANACHANS") {
		SendReply(to_string(g_deviceIsScope ? g_channels.size() : 0));
		return true;
	}
	if (cmd == "DIGCHANS") {
		SendReply(to_string(g_deviceIsScope ? 0 : g_channels.size()));
		return true;
	}

	LogWarning("Unrecognized query received: %s\n", line.c_str());
	return false;
}

bool SigrokSCPIServer::OnCommand(
		const string& line,
		const string& subject,
		const string& cmd,
		const std::vector<std::string>& args)
{
	if (BridgeSCPIServer::OnCommand(line, subject, cmd, args))
		return true;

	// ATTEN is handled on the client side; accept it silently
	size_t channelId;
	if (GetChannelID(subject, channelId) && cmd == "ATTEN")
		return true;

	LogWarning("Unrecognized command received: %s\n", line.c_str());
	return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Acquisition

void SigrokSCPIServer::AcquisitionStart(bool oneShot)
{
	LogDebug("cmd: START%s\n", oneShot ? " (single)" : "");
	g_oneShot = oneShot;
	g_run = true;
}

void SigrokSCPIServer::AcquisitionForceTrigger()
{
	// No software force on these drivers; take one capture
	AcquisitionStart(true);
}

void SigrokSCPIServer::AcquisitionStop()
{
	LogDebug("cmd: STOP\n");
	g_run = false;
	lock_guard<recursive_mutex> lock(g_devMutex);
	abort_running_capture();
}

bool SigrokSCPIServer::IsTriggerArmed()
{
	return g_run;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Channels

void SigrokSCPIServer::SetChannelEnabled(size_t chIndex, bool enabled)
{
	if (set_channel_enabled(chIndex, enabled))
		LogDebug("ch%zu %s\n", chIndex, enabled ? "enabled" : "disabled");
}

void SigrokSCPIServer::SetAnalogCoupling(size_t chIndex, const std::string& coupling)
{
	if (!g_deviceIsScope || chIndex >= g_channels.size())
		return;

	uint8_t sr_coupling;
	if (coupling == "AC1M")
		sr_coupling = SR_AC_COUPLING;
	else if (coupling == "DC1M")
		sr_coupling = SR_DC_COUPLING;
	else {
		LogWarning("Unknown coupling: %s\n", coupling.c_str());
		return;
	}

	lock_guard<recursive_mutex> lock(g_devMutex);
	set_probe_config<uint8_t>(g_channels[chIndex], SR_CONF_PROBE_COUPLING, sr_coupling);
}

void SigrokSCPIServer::SetAnalogRange(size_t chIndex, double range_V)
{
	if (!g_deviceIsScope || chIndex >= g_channels.size() || vdiv_options.empty())
		return;

	// Smallest V/div that still covers the requested range
	double want_mV = range_V / g_numdivs * 1000;
	uint64_t selected = vdiv_options.back();
	for (auto v : vdiv_options) {
		if (v >= want_mV && v < selected)
			selected = v;
	}

	lock_guard<recursive_mutex> lock(g_devMutex);
	set_probe_config<uint64_t>(g_channels[chIndex], SR_CONF_PROBE_VDIV, selected);
	LogDebug("RANGE ch%zu: wanted %.1f mV/div, using %lu\n", chIndex, want_mV, (unsigned long)selected);
}

void SigrokSCPIServer::SetAnalogOffset(size_t /*chIndex*/, double /*offset_V*/)
{
}

void SigrokSCPIServer::SetDigitalThreshold(size_t /*chIndex*/, double threshold_V)
{
	if (g_deviceIsScope)
		return;

	if (threshold_V < 0) threshold_V = 0;
	if (threshold_V > 5) threshold_V = 5;

	// One threshold for the whole device; drivers without SR_CONF_VTH ignore it
	lock_guard<recursive_mutex> lock(g_devMutex);
	if (set_dev_config<double>(SR_CONF_VTH, threshold_V))
		LogDebug("THRESH now %.2f V\n", threshold_V);
}

void SigrokSCPIServer::SetDigitalHysteresis(size_t /*chIndex*/, double /*hysteresis*/)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Timebase and trigger. Changes apply from the next capture.

void SigrokSCPIServer::SetSampleRate(uint64_t rate_hz)
{
	lock_guard<recursive_mutex> lock(g_devMutex);
	abort_running_capture();
	set_rate(rate_hz);
	LogDebug("RATE: wanted %lu, now %lu\n", (unsigned long)rate_hz, (unsigned long)g_rate);
}

void SigrokSCPIServer::SetSampleDepth(uint64_t depth)
{
	lock_guard<recursive_mutex> lock(g_devMutex);
	abort_running_capture();
	set_depth(depth);
	LogDebug("DEPTH: wanted %lu, now %lu\n", (unsigned long)depth, (unsigned long)g_depth);
}

void SigrokSCPIServer::SetTriggerDelay(uint64_t delay_fs)
{
	lock_guard<recursive_mutex> lock(g_devMutex);
	set_trigfs(delay_fs);
	LogDebug("Trigger DELAY %lu fs (%d%%)\n", (unsigned long)delay_fs, g_trigpct);
}

void SigrokSCPIServer::SetTriggerSource(size_t chIndex)
{
	set_trigger_channel(chIndex);
	LogDebug("Trigger SOU %zu\n", chIndex);
}

void SigrokSCPIServer::SetTriggerTypeEdge()
{
}

void SigrokSCPIServer::SetTriggerLevel(double level_V)
{
	if (!g_deviceIsScope)
		return;

	lock_guard<recursive_mutex> lock(g_devMutex);
	for (auto ch : g_channels) {
		float scale, offset;
		compute_scale_and_offset(ch, scale, offset);
		// voltage = ADC * scale - offset  =>  ADC = (voltage + offset) / scale
		float adc = (level_V + offset) / scale;
		if (adc < 0) adc = 0;
		if (adc > 255) adc = 255;
		set_probe_config<uint8_t>(ch, SR_CONF_TRIGGER_VALUE, (uint8_t)adc);
	}
	LogDebug("Trigger LEV %f\n", level_V);
}

void SigrokSCPIServer::SetEdgeTriggerEdge(const std::string& edge)
{
	int dir;
	if (edge == "RISING")
		dir = RISING;
	else if (edge == "FALLING")
		dir = FALLING;
	else if (edge == "ANY" && !g_deviceIsScope)
		dir = ANY;
	else {
		LogWarning("Unsupported trigger EDGE: %s\n", edge.c_str());
		return;
	}

	set_trigger_direction(dir);
	LogDebug("Trigger EDGE %s\n", edge.c_str());
}
