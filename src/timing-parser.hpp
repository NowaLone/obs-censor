#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

struct TimeInterval {
	int start_seconds = 0;
	int end_seconds = 0;
};

inline int timing_clock_to_seconds(int hours, int minutes, int seconds)
{
	return hours * 3600 + minutes * 60 + seconds;
}

inline bool timing_is_digit(char c)
{
	return c >= '0' && c <= '9';
}

inline size_t timing_utf8_len(unsigned char lead)
{
	if ((lead & 0x80) == 0)
		return 1;
	if ((lead & 0xE0) == 0xC0)
		return 2;
	if ((lead & 0xF0) == 0xE0)
		return 3;
	if ((lead & 0xF8) == 0xF0)
		return 4;
	return 1;
}

inline bool timing_starts_with(const std::string &text, size_t i, const char *bytes, size_t n)
{
	return i + n <= text.size() && text.compare(i, n, bytes, n) == 0;
}

inline size_t timing_skip_space(const std::string &text, size_t i)
{
	while (i < text.size()) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
			i++;
			continue;
		}
		// U+00A0 NBSP
		if (timing_starts_with(text, i, "\xC2\xA0", 2)) {
			i += 2;
			continue;
		}
		if (i + 2 < text.size() && c == 0xE2) {
			const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
			const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
			// U+2000-U+200B, U+202F
			if (c1 == 0x80 && ((c2 >= 0x80 && c2 <= 0x8B) || c2 == 0xAF)) {
				i += 3;
				continue;
			}
			// U+205F
			if (c1 == 0x81 && c2 == 0x9F) {
				i += 3;
				continue;
			}
		}
		// U+3000 ideographic space
		if (timing_starts_with(text, i, "\xE3\x80\x80", 3)) {
			i += 3;
			continue;
		}
		break;
	}
	return i;
}

inline bool timing_is_dash(const std::string &text, size_t i, size_t *len)
{
	if (i >= text.size())
		return false;

	const unsigned char c = static_cast<unsigned char>(text[i]);
	if (c == '-') {
		*len = 1;
		return true;
	}

	if (c == 0xE2 && i + 2 < text.size()) {
		const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
		const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
		// U+2010 hyphen .. U+2015 horizontal bar
		if (c1 == 0x80 && c2 >= 0x90 && c2 <= 0x95) {
			*len = 3;
			return true;
		}
		// U+2212 minus
		if (c1 == 0x88 && c2 == 0x92) {
			*len = 3;
			return true;
		}
	}

	return false;
}

inline bool timing_parse_int(const std::string &text, size_t i, int *value, size_t *consumed)
{
	if (i >= text.size() || !timing_is_digit(text[i]))
		return false;

	int n = 0;
	size_t j = i;
	while (j < text.size() && timing_is_digit(text[j])) {
		n = n * 10 + (text[j] - '0');
		if (n > 1000000)
			return false;
		j++;
	}

	*value = n;
	*consumed = j - i;
	return true;
}

inline bool timing_parse_clock(const std::string &text, size_t i, int *total_seconds, size_t *consumed)
{
	int parts[3] = {0, 0, 0};
	int count = 0;
	size_t j = i;

	while (count < 3) {
		int val = 0;
		size_t len = 0;
		if (!timing_parse_int(text, j, &val, &len)) {
			if (count == 0)
				return false;
			break;
		}
		parts[count++] = val;
		j += len;
		if (j < text.size() && text[j] == ':') {
			j++;
			continue;
		}
		break;
	}

	if (count == 2) {
		if (parts[1] >= 60)
			return false;
		*total_seconds = timing_clock_to_seconds(0, parts[0], parts[1]);
		*consumed = j - i;
		return true;
	}

	if (count == 3) {
		if (parts[1] >= 60 || parts[2] >= 60)
			return false;
		*total_seconds = timing_clock_to_seconds(parts[0], parts[1], parts[2]);
		*consumed = j - i;
		return true;
	}

	return false;
}

// Manual parser instead of std::regex: MSVC's regex engine does not backtrack
// optional hour groups in (?:(\d+):)?(\d+):(\d+), so MM:SS - MM:SS ranges
// (e.g. "31:54 - 32:02") match 0 intervals on Windows. Also accepts en/em
// dashes and non-breaking spaces commonly copied from timing websites.
inline std::vector<TimeInterval> parse_timings(const std::string &text)
{
	std::vector<TimeInterval> intervals;
	size_t i = 0;
	const size_t n = text.size();

	while (i < n) {
		int start_seconds = 0;
		int end_seconds = 0;
		size_t start_len = 0;
		size_t end_len = 0;
		size_t dash_len = 0;

		if (!timing_parse_clock(text, i, &start_seconds, &start_len)) {
			i += timing_utf8_len(static_cast<unsigned char>(text[i]));
			continue;
		}

		size_t j = timing_skip_space(text, i + start_len);
		if (!timing_is_dash(text, j, &dash_len)) {
			i += timing_utf8_len(static_cast<unsigned char>(text[i]));
			continue;
		}

		j = timing_skip_space(text, j + dash_len);
		if (!timing_parse_clock(text, j, &end_seconds, &end_len)) {
			i += timing_utf8_len(static_cast<unsigned char>(text[i]));
			continue;
		}

		if (start_seconds > end_seconds)
			std::swap(start_seconds, end_seconds);

		intervals.push_back({start_seconds, end_seconds});
		i = j + end_len;
	}

	return intervals;
}
