/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#elif defined(__linux__)
#include <curl/curl.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kFilterId = "mpc_be_censor_filter";
constexpr const char *kUrlSetting = "mpc_url";
constexpr const char *kPollIntervalSetting = "poll_interval";
constexpr const char *kTimingOffsetSetting = "timing_offset_seconds";
constexpr const char *kTimingsSetting = "timings_text";
constexpr const char *kDefaultMpcUrl = "http://localhost:13579/variables.html";
constexpr int kDefaultPollIntervalMs = 1000;
constexpr int kMinimumPollIntervalMs = 500;
constexpr int kMaximumPollIntervalMs = 5000;
constexpr int kMinimumTimingOffsetSeconds = -86400;
constexpr int kMaximumTimingOffsetSeconds = 86400;

struct TimeInterval {
	int start_seconds = 0;
	int end_seconds = 0;
};

struct FilterData {
	obs_source_t *source = nullptr;

	std::mutex mutex;
	std::condition_variable condition;
	std::thread worker;
	std::vector<TimeInterval> intervals;
	std::string mpc_url = kDefaultMpcUrl;
	int poll_interval_ms = kDefaultPollIntervalMs;
	int timing_offset_seconds = 0;
	bool settings_dirty = false;
	bool stop_requested = false;

	std::atomic_bool should_hide = false;
	std::atomic_int current_time = -1;
	bool logged_state = false;
	bool last_logged_hide = false;
};

int time_to_seconds(int hours, int minutes, int seconds)
{
	return hours * 3600 + minutes * 60 + seconds;
}

int sub_match_to_int(const std::ssub_match &match)
{
	return std::atoi(match.str().c_str());
}

std::string format_offset_time(int total_seconds)
{
	total_seconds = std::clamp(total_seconds, kMinimumTimingOffsetSeconds, kMaximumTimingOffsetSeconds);
	const bool negative = total_seconds < 0;
	const int absolute_seconds = negative ? -total_seconds : total_seconds;
	const int hours = absolute_seconds / 3600;
	const int minutes = (absolute_seconds % 3600) / 60;
	const int seconds = absolute_seconds % 60;
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%s%02d:%02d:%02d", negative ? "-" : "", hours, minutes, seconds);
	return buffer;
}

bool try_parse_offset_string(const std::string &text, int &offset_seconds)
{
	static const std::regex hms_pattern(R"(^\s*([+-])?(\d+):(\d+):(\d+)\s*$)");
	static const std::regex seconds_pattern(R"(^\s*([+-]?\d+)\s*$)");
	std::smatch match;

	if (std::regex_match(text, match, hms_pattern)) {
		const int hours = sub_match_to_int(match[2]);
		const int minutes = sub_match_to_int(match[3]);
		const int seconds = sub_match_to_int(match[4]);

		if (minutes >= 60 || seconds >= 60)
			return false;

		offset_seconds = time_to_seconds(hours, minutes, seconds);
		if (match[1].matched && match[1].str() == "-")
			offset_seconds = -offset_seconds;

		offset_seconds =
			std::clamp(offset_seconds, kMinimumTimingOffsetSeconds, kMaximumTimingOffsetSeconds);
		return true;
	}

	if (std::regex_match(text, match, seconds_pattern)) {
		offset_seconds = std::clamp(std::atoi(match[1].str().c_str()), kMinimumTimingOffsetSeconds,
			kMaximumTimingOffsetSeconds);
		return true;
	}

	return false;
}

int parse_timing_offset_seconds(obs_data_t *settings)
{
	if (!settings)
		return 0;

	const char *raw_value = obs_data_get_string(settings, kTimingOffsetSetting);
	const std::string offset_text = raw_value ? raw_value : "";
	int offset_seconds = 0;

	if (!offset_text.empty()) {
		if (try_parse_offset_string(offset_text, offset_seconds)) {
			const std::string normalized = format_offset_time(offset_seconds);
			obs_data_set_string(settings, kTimingOffsetSetting, normalized.c_str());
			return offset_seconds;
		}

		obs_log(LOG_WARNING, "MPC-BE censor filter: invalid timing offset '%s', using 00:00:00",
			offset_text.c_str());
		obs_data_set_string(settings, kTimingOffsetSetting, "00:00:00");
		return 0;
	}

	offset_seconds = std::clamp(static_cast<int>(obs_data_get_int(settings, kTimingOffsetSetting)),
		kMinimumTimingOffsetSeconds, kMaximumTimingOffsetSeconds);
	const std::string normalized = format_offset_time(offset_seconds);
	obs_data_set_string(settings, kTimingOffsetSetting, normalized.c_str());
	return offset_seconds;
}

std::vector<TimeInterval> parse_timings(const std::string &text)
{
	static const std::regex pattern(R"((\d+):(\d+):(\d+)\s*-\s*(\d+):(\d+):(\d+))");
	std::vector<TimeInterval> intervals;

	for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
		const std::smatch &match = *it;
		int start_seconds =
			time_to_seconds(sub_match_to_int(match[1]), sub_match_to_int(match[2]), sub_match_to_int(match[3]));
		int end_seconds =
			time_to_seconds(sub_match_to_int(match[4]), sub_match_to_int(match[5]), sub_match_to_int(match[6]));

		if (start_seconds > end_seconds)
			std::swap(start_seconds, end_seconds);

		intervals.push_back({start_seconds, end_seconds});
	}

	return intervals;
}

int parse_html_time(const std::string &html)
{
	if (html.empty())
		return -1;

	static const std::regex position_string_pattern(
		R"(<p\s+id="positionstring"[^>]*>\s*(\d+):(\d+):(\d+)\s*</p>)",
		std::regex_constants::icase);
	static const std::regex position_pattern(R"(<p\s+id="position"[^>]*>\s*(\d+)\s*</p>)",
						     std::regex_constants::icase);
	std::smatch match;

	if (std::regex_search(html, match, position_string_pattern)) {
		return time_to_seconds(sub_match_to_int(match[1]), sub_match_to_int(match[2]), sub_match_to_int(match[3]));
	}

	if (std::regex_search(html, match, position_pattern))
		return sub_match_to_int(match[1]) / 1000;

	return -1;
}

bool is_in_interval(const std::vector<TimeInterval> &intervals, int current_time)
{
	for (const TimeInterval &interval : intervals) {
		if (current_time >= interval.start_seconds && current_time <= interval.end_seconds)
			return true;
	}

	return false;
}

#ifdef _WIN32
bool fetch_url(const std::string &url, std::string &response)
{
	response.clear();

	HINTERNET internet = InternetOpenA("OBS MPC-BE Censor", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!internet)
		return false;

	DWORD timeout_ms = 1000;
	InternetSetOptionA(internet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
	InternetSetOptionA(internet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
	InternetSetOptionA(internet, INTERNET_OPTION_SEND_TIMEOUT, &timeout_ms, sizeof(timeout_ms));

	constexpr DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI;
	HINTERNET request = InternetOpenUrlA(internet, url.c_str(), nullptr, 0, flags, 0);
	if (!request) {
		InternetCloseHandle(internet);
		return false;
	}

	char buffer[8192];
	DWORD bytes_read = 0;
	while (InternetReadFile(request, buffer, sizeof(buffer), &bytes_read) && bytes_read > 0)
		response.append(buffer, bytes_read);

	InternetCloseHandle(request);
	InternetCloseHandle(internet);
	return !response.empty();
}
#elif defined(__linux__)
size_t curl_write_callback(char *data, size_t size, size_t count, void *userdata)
{
	auto *response = static_cast<std::string *>(userdata);
	response->append(data, size * count);
	return size * count;
}

bool fetch_url(const std::string &url, std::string &response)
{
	response.clear();

	CURL *curl = curl_easy_init();
	if (!curl)
		return false;

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);

	const CURLcode result = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return result == CURLE_OK && !response.empty();
}
#else
bool fetch_url(const std::string &url, std::string &response)
{
	static_cast<void>(url);
	response.clear();
	return false;
}
#endif

int fetch_mpc_time_seconds(const std::string &url)
{
	if (url.empty())
		return -1;

	std::string response;
	if (!fetch_url(url, response))
		return -1;

	return parse_html_time(response);
}

const char *filter_get_name(void *)
{
	return obs_module_text("MpcBeCensorFilter.Name");
}

void log_visibility_change(bool should_hide, int current_time)
{
	const int hours = current_time / 3600;
	const int minutes = (current_time % 3600) / 60;
	const int seconds = current_time % 60;
	const char *state = should_hide ? "hidden" : "shown";

	obs_log(LOG_INFO, "MPC-BE censor filter: source %s at %02d:%02d:%02d", state, hours, minutes, seconds);
}

void update_runtime_settings(FilterData *filter, obs_data_t *settings)
{
	if (!filter)
		return;

	std::vector<TimeInterval> intervals = parse_timings(obs_data_get_string(settings, kTimingsSetting));
	std::string mpc_url = obs_data_get_string(settings, kUrlSetting);
	int poll_interval_ms = static_cast<int>(obs_data_get_int(settings, kPollIntervalSetting));
	int timing_offset_seconds = parse_timing_offset_seconds(settings);
	poll_interval_ms = std::clamp(poll_interval_ms, kMinimumPollIntervalMs, kMaximumPollIntervalMs);
	const bool disable_hiding = mpc_url.empty() || intervals.empty();
	const size_t interval_count = intervals.size();
	const std::string normalized_offset = format_offset_time(timing_offset_seconds);

	{
		std::lock_guard<std::mutex> lock(filter->mutex);
		filter->intervals = std::move(intervals);
		filter->mpc_url = std::move(mpc_url);
		filter->poll_interval_ms = poll_interval_ms;
		filter->timing_offset_seconds = timing_offset_seconds;
		filter->settings_dirty = true;
		filter->logged_state = false;
	}

	if (disable_hiding) {
		filter->should_hide.store(false, std::memory_order_relaxed);
		filter->current_time.store(-1, std::memory_order_relaxed);
	}

	filter->condition.notify_all();
	obs_log(LOG_INFO, "MPC-BE censor filter: loaded %d timing interval(s), offset=%s",
		static_cast<int>(interval_count), normalized_offset.c_str());
}

void worker_loop(FilterData *filter)
{
	std::unique_lock<std::mutex> lock(filter->mutex);

	while (!filter->stop_requested) {
		const std::vector<TimeInterval> intervals = filter->intervals;
		const std::string mpc_url = filter->mpc_url;
		const int poll_interval_ms = filter->poll_interval_ms;
		const int timing_offset_seconds = filter->timing_offset_seconds;
		filter->settings_dirty = false;

		if (intervals.empty() || mpc_url.empty()) {
			filter->condition.wait(lock, [filter]() { return filter->stop_requested || filter->settings_dirty; });
			continue;
		}

		lock.unlock();

		const int current_time = fetch_mpc_time_seconds(mpc_url);
		if (current_time >= 0) {
			const int adjusted_time = current_time - timing_offset_seconds;
			const bool should_hide = !is_in_interval(intervals, adjusted_time);
			filter->should_hide.store(should_hide, std::memory_order_relaxed);
			filter->current_time.store(current_time, std::memory_order_relaxed);

			bool should_log = false;
			{
				std::lock_guard<std::mutex> state_lock(filter->mutex);
				if (!filter->logged_state || filter->last_logged_hide != should_hide) {
					filter->logged_state = true;
					filter->last_logged_hide = should_hide;
					should_log = true;
				}
			}

			if (should_log)
				log_visibility_change(should_hide, current_time);
		} else {
			filter->current_time.store(-1, std::memory_order_relaxed);
		}

		lock.lock();
		filter->condition.wait_for(lock, std::chrono::milliseconds(poll_interval_ms),
			[filter]() { return filter->stop_requested || filter->settings_dirty; });
	}
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new FilterData();
	filter->source = source;
	update_runtime_settings(filter, settings);
	filter->worker = std::thread(worker_loop, filter);
	return filter;
}

void filter_destroy(void *data)
{
	auto *filter = static_cast<FilterData *>(data);
	if (!filter)
		return;

	{
		std::lock_guard<std::mutex> lock(filter->mutex);
		filter->stop_requested = true;
		filter->settings_dirty = true;
	}

	filter->condition.notify_all();
	if (filter->worker.joinable())
		filter->worker.join();

	delete filter;
}

void filter_update(void *data, obs_data_t *settings)
{
	update_runtime_settings(static_cast<FilterData *>(data), settings);
}

void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, kUrlSetting, kDefaultMpcUrl);
	obs_data_set_default_int(settings, kPollIntervalSetting, kDefaultPollIntervalMs);
	obs_data_set_default_string(settings, kTimingOffsetSetting, "00:00:00");
	obs_data_set_default_string(settings, kTimingsSetting, "");
}

obs_properties_t *filter_properties(void *)
{
	obs_properties_t *properties = obs_properties_create();
	obs_properties_add_text(properties, kUrlSetting, obs_module_text("MpcBeCensorFilter.Url"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(properties, kPollIntervalSetting, obs_module_text("MpcBeCensorFilter.PollInterval"),
		kMinimumPollIntervalMs, kMaximumPollIntervalMs, 100);
	obs_properties_add_text(properties, kTimingOffsetSetting, obs_module_text("MpcBeCensorFilter.Offset"),
		OBS_TEXT_DEFAULT);
	obs_properties_add_text(properties, kTimingsSetting, obs_module_text("MpcBeCensorFilter.Timings"),
		OBS_TEXT_MULTILINE);
	return properties;
}

void filter_video_render(void *data, gs_effect_t *effect)
{
	auto *filter = static_cast<FilterData *>(data);
	static_cast<void>(effect);

	if (!filter)
		return;

	if (filter->should_hide.load(std::memory_order_relaxed))
		return;

	obs_source_skip_video_filter(filter->source);
}

obs_source_info *get_filter_info()
{
	static obs_source_info info = []() {
		obs_source_info value = {};
		value.id = kFilterId;
		value.type = OBS_SOURCE_TYPE_FILTER;
		value.output_flags = OBS_SOURCE_VIDEO;
		value.get_name = filter_get_name;
		value.create = filter_create;
		value.destroy = filter_destroy;
		value.get_defaults = filter_defaults;
		value.get_properties = filter_properties;
		value.update = filter_update;
		value.video_render = filter_video_render;
		return value;
	}();

	return &info;
}

} // namespace

bool obs_module_load(void)
{
#if defined(__linux__)
	curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
	obs_register_source(get_filter_info());
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
#if defined(__linux__)
	curl_global_cleanup();
#endif
	obs_log(LOG_INFO, "plugin unloaded");
}
