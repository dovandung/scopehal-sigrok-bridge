#include "srbinding.h"

#include <stdint.h>
#include <stdio.h>
#include <glib.h>
#include <map>

#define BINDING_TYPES_X(X) \
 X(uint64_t, UINT64, uint64) \
 X(uint32_t, UINT32, uint32) \
 X(uint16_t, UINT16, uint16) \
 X(int16_t, INT16, int16) \
 X(int32_t, INT32, int32) \
 X(uint8_t, BYTE, byte) \
 X(bool, BOOLEAN, boolean) \
 X(double, DOUBLE, double) \
 X(std::string, STRING, string)

template <typename T>
bool extract_gvar(GVariant* gvar, T& result);

template <typename T>
GVariant* make_gvar(T value);

#define g_variant_get_string(X) std::string(g_variant_get_string(X, NULL))
#define g_variant_new_string(X) g_variant_new_string(X.c_str())

// extract_gvar() takes ownership of gvar and always releases it.
#define X_MAKE_SPEC(CTYPE, GTYPELIT, GTYPEFUNC) \
template <> \
bool extract_gvar<CTYPE>(GVariant* gvar, CTYPE& result) { \
	if (gvar == NULL) return false; \
	bool ok = g_variant_is_of_type(gvar, G_VARIANT_TYPE_##GTYPELIT); \
	if (ok) result = g_variant_get_##GTYPEFUNC(gvar); \
	g_variant_unref(gvar); \
	return ok; \
} \
template <> \
GVariant* make_gvar<CTYPE>(CTYPE value) { \
	return g_variant_new_##GTYPEFUNC(value); \
}

BINDING_TYPES_X(X_MAKE_SPEC)

template <typename T>
std::optional<T> get_probe_config(const struct sr_channel* ch, int key) {
	GVariant* gvar = NULL;
	if (ds_get_actived_device_config(ch, NULL, key, &gvar) != SR_OK)
		return std::nullopt;

	T result;
	if (extract_gvar<T>(gvar, result))
		return std::optional{result};

	return std::nullopt;
}

template <typename T>
bool set_probe_config(const struct sr_channel* ch, int key, T value) {
	GVariant* gvar = g_variant_ref_sink(make_gvar<T>(value));
	int err = ds_set_actived_device_config(ch, NULL, key, gvar);
	g_variant_unref(gvar);
	return err == SR_OK;
}

// Some list keys return a dictionary; the actual values sit under this name.
static std::map<int, std::string> dev_config_indirections {
	{SR_CONF_PROBE_VDIV, "vdivs"},
	{SR_CONF_SAMPLERATE, "samplerates"}
};

template <typename T>
std::vector<T> get_dev_config_options(int key) {
	GVariant* list = NULL;
	GVariant* orig_list = NULL;
	std::vector<T> values;

	if (ds_get_actived_device_config_list(NULL, key, &list) != SR_OK || list == NULL)
		return values;

	auto ind = dev_config_indirections.find(key);
	if (ind != dev_config_indirections.end() && g_variant_is_of_type(list, G_VARIANT_TYPE_DICTIONARY)) {
		orig_list = list;
		list = g_variant_lookup_value(orig_list, ind->second.c_str(), NULL);
		if (list == NULL) {
			g_variant_unref(orig_list);
			return values;
		}
	}

	if (g_variant_is_container(list)) {
		GVariantIter iterator;
		g_variant_iter_init(&iterator, list);
		GVariant* item;
		while ((item = g_variant_iter_next_value(&iterator)) != NULL) {
			T result;
			if (extract_gvar<T>(item, result))
				values.push_back(result);
		}
	}

	g_variant_unref(list);
	if (orig_list)
		g_variant_unref(orig_list);

	return values;
}

#define X_FORCE_INSTANTIATION(CTYPE, GTYPELIT, GTYPEFUNC) \
template std::optional<CTYPE> get_probe_config<CTYPE>(const struct sr_channel* ch, int key); \
template bool set_probe_config<CTYPE>(const struct sr_channel* ch, int key, CTYPE value); \
template std::vector<CTYPE> get_dev_config_options<CTYPE>(int key);

BINDING_TYPES_X(X_FORCE_INSTANTIATION)
