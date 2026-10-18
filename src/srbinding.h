#ifndef SRBINDING_H
#define SRBINDING_H

// Typed wrappers around the libsigrok4DSL ds_*_actived_device_config() API.
// Every call acts on the device activated with ds_active_device().

#include <libsigrok.h>
#include <optional>
#include <string>
#include <vector>

template <typename T>
std::optional<T> get_probe_config(const struct sr_channel* ch, int key);

template <typename T>
bool set_probe_config(const struct sr_channel* ch, int key, T value);

template <typename T>
inline std::optional<T> get_dev_config(int key) {
	return get_probe_config<T>(NULL, key);
}

template <typename T>
inline bool set_dev_config(int key, T value) {
	return set_probe_config<T>(NULL, key, value);
}

template <typename T>
std::vector<T> get_dev_config_options(int key);

#endif
