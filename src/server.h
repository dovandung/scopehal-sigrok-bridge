#ifndef server_h
#define server_h

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <libsigrok.h>
#include "xptools/Socket.h"

extern Socket* g_scpiSocket;
extern Socket* g_dataSocket;

// Active device description
extern std::string g_devName;		// name reported by the driver, e.g. "SLogic16U3"
extern std::string g_driverName;	// libsigrok4DSL driver name, e.g. "slogic16u3"
extern bool g_deviceIsScope;		// DSO mode (DSCope) vs logic analyzer
extern std::vector<struct sr_channel*> g_channels;

// Configuration cache
extern uint64_t g_rate, g_depth, g_trigfs, g_maxDepth;
extern uint8_t g_trigpct;
extern int g_selectedTriggerChannel;
extern int g_selectedTriggerDirection;
extern std::vector<uint64_t> vdiv_options;
extern int g_numdivs;
extern uint32_t g_hwmin, g_hwmax;
extern float g_hwrange_factor;

// Acquisition state
extern std::atomic<bool> g_quit;	// tear down the current client
extern std::atomic<bool> g_run;		// client wants captures
extern std::atomic<bool> g_oneShot;

// Serializes ds_start_collect/ds_stop_collect and device reconfiguration
extern std::recursive_mutex g_devMutex;
extern int g_requestedMode;		// -1: keep the device's mode

enum trigger_direction {
	RISING,
	FALLING,
	ANY,
	NONE
};

int list_devices();
int init_and_find_device(const std::string& selector);
void shutdown_device();

void compute_scale_and_offset(struct sr_channel* ch, float& scale, float& offset);
int count_enabled_channels();
std::vector<size_t> enabled_channel_indexes();

void set_trigger_channel(int ch);
void set_trigger_direction(int direction);
void set_rate(uint64_t rate);
void set_depth(uint64_t depth);
void set_trigfs(uint64_t fs);
bool set_channel_enabled(size_t ch, bool enabled);

// Stop a running hardware capture so the device can be reconfigured.
// Must be called with g_devMutex held. The acquisition thread restarts
// capturing on its own while g_run is set.
void abort_running_capture();

// Acquisition and data plane (Capture.cpp)
void InstallCallbacks();
void AcquisitionThread();
void WaveformServerThread();
bool IsCapturing();

#endif // server_h
