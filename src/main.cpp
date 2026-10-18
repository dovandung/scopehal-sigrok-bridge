#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <string>
#include <thread>

#include <libsigrok.h>

#include "log/log.h"

#include "server.h"
#include "SigrokSCPIServer.h"

Socket* g_scpiSocket = NULL;
Socket* g_dataSocket = NULL;

// Listen on IPv6 (dual stack) when available, else IPv4
static Socket* make_listener(int port)
{
	for (int af : {AF_INET6, AF_INET}) {
		if (af == AF_INET6) {
			int probe = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
			if (probe < 0)
				continue;	// no IPv6 on this host
			::close(probe);
		}
		auto s = new Socket(af, SOCK_STREAM, IPPROTO_TCP);
		s->SetReuseaddr();
		if (s->Bind(port) && s->Listen())
			return s;
		delete s;
	}
	return NULL;
}

static void usage(const char* argv0)
{
	printf(
		"Usage: %s [options] <device>\n"
		"\n"
		"Exposes a libsigrok4DSL (DSView / ALL-LOGIC) device to ngscopeclient.\n"
		"<device> is an index, part of a device name, or a driver name from --list.\n"
		"\n"
		"Options:\n"
		"  --list              list detected devices and exit\n"
		"  --port N            SCPI control port (default 5025; data port is N+1)\n"
		"  --res DIR           firmware/bitstream directory (default /usr/local/share/DSView/res)\n"
		"  --mode logic|dso    switch the device to this mode first (DSCope, demo)\n"
		"  --max-depth N       largest memory depth offered to the client (default 100000000)\n"
		"  --sr-log N          libsigrok4DSL log level 0-5 (default 2)\n"
		"  -q, --quiet         less console output\n",
		argv0);
}

int main(int argc, char* argv[])
{
	std::string selector;
	std::string resdir = "/usr/local/share/DSView/res/";
	int scpi_port = 5025;
	int srlog = 2;
	bool list = false;
	Severity console_verbosity = Severity::DEBUG;

	for (int i = 1; i < argc; i++) {
		std::string s = argv[i];
		auto next = [&](const char* what) -> std::string {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s needs %s\n", s.c_str(), what);
				exit(1);
			}
			return argv[++i];
		};
		if (s == "--list")
			list = true;
		else if (s == "--port")
			scpi_port = atoi(next("a port number").c_str());
		else if (s == "--res")
			resdir = next("a directory");
		else if (s == "--mode") {
			std::string m = next("logic or dso");
			if (m == "logic") g_requestedMode = LOGIC;
			else if (m == "dso") g_requestedMode = DSO;
			else { fprintf(stderr, "--mode must be logic or dso\n"); return 1; }
		}
		else if (s == "--max-depth")
			g_maxDepth = strtoull(next("a sample count").c_str(), NULL, 10);
		else if (s == "--sr-log")
			srlog = atoi(next("a level").c_str());
		else if (s == "-q" || s == "--quiet")
			console_verbosity = Severity::NOTICE;
		else if (s == "-h" || s == "--help") {
			usage(argv[0]);
			return 0;
		} else if (!s.empty() && s[0] == '-') {
			fprintf(stderr, "Unknown option %s\n", s.c_str());
			usage(argv[0]);
			return 1;
		} else
			selector = s;
	}

	if (!list && selector.empty()) {
		usage(argv[0]);
		return 1;
	}

	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));
	signal(SIGPIPE, SIG_IGN);

	if (resdir.back() != '/')
		resdir += '/';
	ds_log_level(srlog);
	ds_set_firmware_resource_dir(resdir.c_str());
	ds_set_user_data_dir("/tmp");
	InstallCallbacks();

	if (ds_lib_init() != SR_OK) {
		LogError("ds_lib_init failed\n");
		return 1;
	}
	LogNotice("libsigrok4DSL %s\n", sr_get_lib_version_string());

	if (list) {
		int ret = list_devices();
		ds_lib_exit();
		return ret;
	}

	if (init_and_find_device(selector) != 0) {
		ds_lib_exit();
		return 1;
	}

	int waveform_port = scpi_port + 1;

	g_dataSocket = make_listener(waveform_port);
	if (!g_dataSocket) {
		LogError("Cannot listen on data port %d\n", waveform_port);
		return 1;
	}

	g_scpiSocket = make_listener(scpi_port);
	if (!g_scpiSocket) {
		LogError("Cannot listen on SCPI port %d\n", scpi_port);
		return 1;
	}

	LogNotice("Ready: SCPI on port %d, waveforms on port %d\n", scpi_port, waveform_port);

	while (true) {
		Socket scpiClient = g_scpiSocket->Accept();
		if (!scpiClient.IsValid())
			break;
		LogNotice("Client connected\n");

		g_quit = false;
		g_run = false;

		SigrokSCPIServer server(scpiClient.Detach());

		std::thread acqThread(AcquisitionThread);
		std::thread dataThread(WaveformServerThread);

		server.MainLoop();

		// Client went away: stop capturing and tear down this session
		g_run = false;
		g_quit = true;
		{
			std::lock_guard<std::recursive_mutex> lock(g_devMutex);
			abort_running_capture();
		}

		acqThread.join();
		dataThread.join();
		LogNotice("Client disconnected, waiting for the next one\n");
	}

	shutdown_device();
	return 0;
}
