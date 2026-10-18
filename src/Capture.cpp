// Acquisition loop and data plane.
//
// libsigrok4DSL delivers one capture as a stream of packets on its own collect
// thread: SR_DF_HEADER, any number of SR_DF_LOGIC (or SR_DF_DSO) chunks, an
// optional SR_DF_TRIGGER between them, then SR_DF_END. Community drivers send
// many chunks per capture, so packets are accumulated here and a waveform is
// published only once the capture is complete.

#include "server.h"
#include "srbinding.h"
#include "log/log.h"
#include "xptools/HzClock.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <thread>

#include <poll.h>

std::atomic<bool> g_quit{false};
std::atomic<bool> g_run{false};
std::atomic<bool> g_oneShot{false};

namespace {

struct Capture {
	bool dso = false;
	uint64_t rate = 0;
	std::vector<size_t> chans;			// channel indexes present in the data
	std::vector<std::vector<uint8_t>> data;		// per channel; logic: 8 samples per byte, LSB first
	uint64_t samples = 0;				// samples per channel received so far
	bool triggered = false;
	uint64_t trigSample = 0;			// sample index of the trigger, if triggered
	bool complete = false;
	bool error = false;
	std::vector<bool> clipping;
	unsigned packets = 0;			// packets received (diagnostics)
	uint64_t configGen = 0;			// g_configGen when the capture started
};

std::mutex g_stateMutex;
std::condition_variable g_stateCv;

std::shared_ptr<Capture> g_cur;			// being filled by the collect thread
bool g_collectEnded = false;
GThread* g_collectThread = NULL;		// lib's collect thread, joined by us
bool g_discard = false;				// capture aborted for reconfiguration
uint64_t g_configGen = 0;			// bumped on every reconfiguration
bool g_stopAfterFrame = false;			// DSO single shot: stop the running collect
bool g_libJoined = false;			// ds_stop_collect() already joined the thread
std::atomic<bool> g_capturing{false};

std::shared_ptr<Capture> g_ready;		// latest complete capture, waiting for the client
uint32_t g_seqnum = 0;
HzClock g_hwRateClock;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Callbacks from libsigrok4DSL (collect thread)

void on_event(int event) {
	std::lock_guard<std::mutex> lock(g_stateMutex);
	switch (event) {
	case DS_EV_COLLECT_TASK_START:
		// Runs on the collect thread. The library never joins this thread when
		// a capture ends by itself, so we keep it and join it ourselves.
		g_collectThread = g_thread_self();
		break;
	case DS_EV_COLLECT_TASK_END:
	case DS_EV_COLLECT_TASK_END_BY_ERROR:
	case DS_EV_COLLECT_TASK_END_BY_DETACHED:
		if (event != DS_EV_COLLECT_TASK_END && g_cur)
			g_cur->error = true;
		g_collectEnded = true;
		g_stateCv.notify_all();
		break;
	default:
		break;
	}
}

void init_capture(Capture& c) {
	c.chans = enabled_channel_indexes();
	c.data.assign(c.chans.size(), {});
	c.clipping.assign(c.chans.size(), false);
	c.samples = 0;
	c.triggered = false;
	c.rate = g_rate;
	c.dso = g_deviceIsScope;
}

uint64_t trigger_sample_from_packet(const Capture& c, const struct ds_trigger_pos* pos) {
	// PXLogic sends SR_DF_TRIGGER without payload, at the trigger point
	if (pos == NULL || !(pos->status & 1))
		return c.samples;

	if (g_driverName.rfind("DSLogic", 0) == 0) {
		// DSLogic reports the position in 16-bit units across enabled channels
		int en = std::max<int>(1, c.chans.size());
		return (uint64_t)pos->real_pos * 16 / en;
	}
	// capture_core based drivers (FX3, SLogic, hsdaoh, ...) report the sample index
	return pos->real_pos;
}

void on_datafeed(const struct sr_dev_inst* /*sdi*/, const struct sr_datafeed_packet* packet) {
	std::lock_guard<std::mutex> lock(g_stateMutex);
	if (!g_cur)
		return;
	Capture& c = *g_cur;
	c.packets++;

	switch (packet->type) {
	case SR_DF_HEADER:
		// Not every driver sends a header (the demo driver does not), so the
		// capture is also initialized when collection starts.
		if (c.samples == 0)
			init_capture(c);
		break;

	case SR_DF_TRIGGER:
		c.triggered = true;
		c.trigSample = trigger_sample_from_packet(c, (const struct ds_trigger_pos*)packet->payload);
		break;

	case SR_DF_LOGIC: {
		auto logic = (const struct sr_datafeed_logic*)packet->payload;
		if (logic == NULL || logic->data == NULL || c.chans.empty())
			break;
		// data_error is ignored, as DSView does: some drivers (e.g. demo)
		// leave the field uninitialized.
		if (logic->format != LA_CROSS_DATA) {
			LogWarning("SR_DF_LOGIC: unsupported format %d\n", logic->format);
			c.error = true;
			break;
		}

		// LA_CROSS_DATA: for each group of 64 samples, 8 bytes per enabled
		// channel in channel order. Bit i of the little-endian 64-bit word is
		// sample i, so byte k bit b is sample 8k+b.
		const size_t n = c.chans.size();
		const size_t groupBytes = 8 * n;
		const size_t groups = logic->length / groupBytes;
		// A trailing partial group (demo driver) cannot be split per channel; drop it.
		static bool warned = false;
		if ((logic->length % groupBytes) && !warned) {
			warned = true;
			LogDebug("SR_DF_LOGIC: %lu bytes is not a whole number of %zu-byte groups; tail dropped\n",
				(unsigned long)logic->length, groupBytes);
		}

		const uint8_t* p = (const uint8_t*)logic->data;
		for (size_t ch = 0; ch < n; ch++)
			c.data[ch].reserve(c.data[ch].size() + groups * 8);
		for (size_t g = 0; g < groups; g++)
			for (size_t ch = 0; ch < n; ch++, p += 8)
				c.data[ch].insert(c.data[ch].end(), p, p + 8);
		c.samples += groups * 64;
		break;
	}

	case SR_DF_DSO: {
		// DSCope keeps one collect running and sends each frame as a single
		// SR_DF_DSO packet (no SR_DF_END per frame), so publish frames directly.
		auto dso = (const struct sr_datafeed_dso*)packet->payload;
		if (dso == NULL || dso->data == NULL || c.chans.empty() || !g_run)
			break;
		if (c.configGen != g_configGen || g_stopAfterFrame)
			break;

		auto f = std::make_shared<Capture>();
		f->dso = true;
		f->rate = c.rate;
		f->chans = c.chans;
		f->configGen = c.configGen;
		const size_t n = f->chans.size();
		f->data.assign(n, std::vector<uint8_t>(dso->num_samples));
		f->clipping.assign(n, false);
		const uint8_t* p = (const uint8_t*)dso->data;
		for (int s = 0; s < dso->num_samples; s++) {
			for (size_t ch = 0; ch < n; ch++) {
				uint8_t d = *p++;
				if (d <= g_hwmin || d >= g_hwmax)
					f->clipping[ch] = true;
				f->data[ch][s] = d;
			}
		}
		f->samples = dso->num_samples;
		f->complete = true;

		g_hwRateClock.Tick();
		g_ready = f;
		if (g_oneShot)
			g_stopAfterFrame = true;
		g_stateCv.notify_all();
		break;
	}

	case SR_DF_END:
		c.complete = true;
		break;

	default:
		break;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Helpers

// Linear interpolation of the trigger crossing for DSO captures, in samples
float InterpolateTriggerPhase(const std::vector<uint8_t>& buf, uint8_t trigvalue, size_t trigpos) {
	for (int i = 0; i < 10; i++) {
		long at = (long)trigpos + ((i & 1) ? -(i + 1) / 2 : i / 2);
		if (at <= 0 || (size_t)at >= buf.size())
			continue;
		float pre = buf[at - 1], aft = buf[at];
		float slope = aft - pre;
		if (slope == 0)
			continue;
		float phase = (trigvalue - pre) / slope;
		float val = -(1 - phase);
		if (val > -1 && val <= 0)
			return val + (at - (long)trigpos);
	}
	return 0;
}

void send_capture(Socket& client, const Capture& c) {
	uint32_t seqnum = g_seqnum++;
	uint16_t numchans = c.chans.size();
	uint64_t rate = c.rate ? c.rate : 1;
	if (c.dso && rate == 1000000000 && numchans == 2)
		rate /= 2;
	int64_t fs_per_sample = 1000000000000000LL / rate;
	int64_t trig_fs = g_trigfs;
	double wfms_s = g_hwRateClock.GetAverageHz();

	client.SendLooped((uint8_t*)&seqnum, sizeof(seqnum));
	client.SendLooped((uint8_t*)&numchans, sizeof(numchans));
	client.SendLooped((uint8_t*)&fs_per_sample, sizeof(fs_per_sample));
	client.SendLooped((uint8_t*)&trig_fs, sizeof(trig_fs));
	client.SendLooped((uint8_t*)&wfms_s, sizeof(wfms_s));

	// Where the trigger should sit, and where it actually is
	uint64_t nominal = fs_per_sample ? trig_fs / fs_per_sample : 0;
	uint64_t actual = c.triggered ? c.trigSample : nominal;

	float trigphase = 0;
	if (c.dso && g_selectedTriggerChannel < (int)c.chans.size()) {
		size_t ti = g_selectedTriggerChannel;
		trigphase = InterpolateTriggerPhase(c.data[ti], g_channels[c.chans[ti]]->trig_value,
			c.samples * g_trigpct / 100);
	}

	for (size_t i = 0; i < c.chans.size(); i++) {
		size_t chnum = c.chans[i];
		size_t memdepth = c.data[i].size();	// logic: bytes (8 samples each)
		client.SendLooped((uint8_t*)&chnum, sizeof(chnum));
		client.SendLooped((uint8_t*)&memdepth, sizeof(memdepth));

		if (c.dso) {
			float scale, offset;
			compute_scale_and_offset(g_channels[chnum], scale, offset);
			float config[3] = {scale, offset, trigphase};
			client.SendLooped((uint8_t*)&config, sizeof(config));
			bool clip = c.clipping[i];
			client.SendLooped((uint8_t*)&clip, sizeof(clip));
		} else {
			int32_t first_sample = (int32_t)((int64_t)nominal - (int64_t)actual);
			client.SendLooped((uint8_t*)&first_sample, sizeof(first_sample));
		}
		client.SendLooped(c.data[i].data(), memdepth);
	}
}

} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////
// Public

void InstallCallbacks() {
	ds_set_event_callback(on_event);
	ds_set_datafeed_callback(on_datafeed);
}

bool IsCapturing() {
	return g_capturing;
}

// Called with g_devMutex held.
void abort_running_capture() {
	// A waveform captured with the old settings must not reach the client
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_ready.reset();
		g_configGen++;
	}
	if (!ds_is_collecting())
		return;
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_discard = true;
	}
	if (ds_stop_collect() == SR_OK) {
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_libJoined = true;
	}
}

void AcquisitionThread() {
	pthread_setname_np(pthread_self(), "Acquisition");

	int failures = 0;
	while (!g_quit) {
		if (!g_run) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		{
			std::lock_guard<std::recursive_mutex> dev(g_devMutex);
			{
				std::lock_guard<std::mutex> lock(g_stateMutex);
				g_cur = std::make_shared<Capture>();
				init_capture(*g_cur);
				g_cur->configGen = g_configGen;
				g_collectEnded = false;
				g_collectThread = NULL;
				g_discard = false;
				g_libJoined = false;
				g_stopAfterFrame = false;
			}
			int err = ds_start_collect();
			if (err != SR_OK) {
				if (++failures % 50 == 1)
					LogError("ds_start_collect failed: %d (%s)\n", err, sr_error_str(err));
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			g_capturing = true;
		}

		// Wait for the capture to finish. A stop request ends it through
		// ds_stop_collect(), which also delivers the END event.
		std::shared_ptr<Capture> done;
		GThread* thread;
		{
			std::unique_lock<std::mutex> lock(g_stateMutex);
			g_stateCv.wait(lock, []{ return g_collectEnded || g_stopAfterFrame; });
			if (g_stopAfterFrame && !g_collectEnded) {
				// DSO single shot: the frame is published, stop the device.
				// ds_stop_collect() joins the collect thread and waits for it,
				// so release our lock while it runs.
				lock.unlock();
				g_run = false;
				{
					std::lock_guard<std::recursive_mutex> dev(g_devMutex);
					if (ds_stop_collect() == SR_OK) {
						std::lock_guard<std::mutex> l2(g_stateMutex);
						g_libJoined = true;
					}
				}
				lock.lock();
				g_stateCv.wait(lock, []{ return g_collectEnded; });
			}
			done = g_cur;
			g_cur.reset();
			thread = g_collectThread;
		}

		// Join the collect thread unless ds_stop_collect() already did.
		bool discard;
		{
			std::lock_guard<std::recursive_mutex> dev(g_devMutex);
			std::lock_guard<std::mutex> lock(g_stateMutex);
			if (thread && !g_libJoined)
				g_thread_join(thread);
			g_collectThread = NULL;
			// Also drop captures that finished just before a reconfiguration
			discard = g_discard || (done && done->configGen != g_configGen);
			g_capturing = false;
		}

		if (done && done->dso)
			continue;	// DSO frames are published from the data callback

		if (discard || !done || !done->complete || done->error || done->samples == 0) {
			if (!discard && done)
				LogDebug("Capture dropped: complete=%d error=%d samples=%lu packets=%u\n",
					done->complete, done->error, (unsigned long)done->samples, done->packets);
			if (done && done->error) {
				if (++failures % 50 == 1)
					LogWarning("Capture ended with an error\n");
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			continue;
		}
		failures = 0;

		g_hwRateClock.Tick();
		{
			std::lock_guard<std::mutex> lock(g_stateMutex);
			g_ready = done;
		}
		g_stateCv.notify_all();

		if (g_oneShot)
			g_run = false;
	}
}

void WaveformServerThread() {
	pthread_setname_np(pthread_self(), "WaveformServer");

	// Wait for the data connection, giving up if the control client leaves
	for (;;) {
		if (g_quit)
			return;
		struct pollfd pfd = { (ZSOCKET)*g_dataSocket, POLLIN, 0 };
		if (::poll(&pfd, 1, 100) > 0)
			break;
	}
	Socket client = g_dataSocket->Accept();
	if (!client.IsValid())
		return;
	LogVerbose("Client connected to data plane socket\n");
	if (!client.DisableNagle())
		LogWarning("Failed to disable Nagle on socket, performance may be poor\n");

	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_ready.reset();
	}
	g_seqnum = 0;
	g_hwRateClock.Reset();

	// The client sends 'K' each time it wants a waveform
	std::atomic<int> requests{0};
	std::atomic<bool> disconnected{false};
	std::thread reader([&]{
		for (;;) {
			uint8_t r = 0;
			if (!client.RecvLooped(&r, 1) || r != 'K')
				break;
			requests++;
			g_stateCv.notify_all();
		}
		disconnected = true;
		g_stateCv.notify_all();
	});

	while (!g_quit && !disconnected) {
		std::shared_ptr<Capture> wfm;
		{
			std::unique_lock<std::mutex> lock(g_stateMutex);
			g_stateCv.wait_for(lock, std::chrono::milliseconds(100),
				[&]{ return g_quit || disconnected || (requests > 0 && g_ready); });
			if (requests > 0 && g_ready) {
				wfm = g_ready;
				g_ready.reset();
				requests--;
			}
		}
		if (wfm)
			send_capture(client, *wfm);
	}

	// Unblock the reader (close() alone does not wake a blocked recv())
	::shutdown((ZSOCKET)client, SHUT_RDWR);
	reader.join();
}
