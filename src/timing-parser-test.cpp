#include "timing-parser.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int g_failed = 0;

static void expect_count(const char *name, const std::string &text, size_t expected)
{
	const auto intervals = parse_timings(text);
	if (intervals.size() != expected) {
		std::printf("FAIL %s: expected %zu interval(s), got %zu\n", name, expected, intervals.size());
		g_failed++;
		return;
	}
	std::printf("OK   %s (%zu)\n", name, expected);
}

static void expect_range(const char *name, const std::string &text, int start, int end)
{
	const auto intervals = parse_timings(text);
	if (intervals.size() != 1 || intervals[0].start_seconds != start || intervals[0].end_seconds != end) {
		std::printf("FAIL %s: expected 1 interval %d-%d, got %zu", name, start, end, intervals.size());
		if (!intervals.empty())
			std::printf(" first=%d-%d", intervals[0].start_seconds, intervals[0].end_seconds);
		std::printf("\n");
		g_failed++;
		return;
	}
	std::printf("OK   %s (%d-%d)\n", name, start, end);
}

int main()
{
	expect_range("mmss spaces", "31:54 - 32:02", 31 * 60 + 54, 32 * 60 + 2);
	expect_range("mmss no spaces", "31:54-32:02", 31 * 60 + 54, 32 * 60 + 2);
	expect_range("mmss space after dash", "31:54- 32:02", 31 * 60 + 54, 32 * 60 + 2);
	expect_range("mmss space before dash", "31:54 -32:02", 31 * 60 + 54, 32 * 60 + 2);
	expect_range("screenshot line", "30:20 - 30:27 - грудь", 30 * 60 + 20, 30 * 60 + 27);
	expect_range("hms", "00:03:45-00:03:49", 3 * 60 + 45, 3 * 60 + 49);
	expect_range("hms with note", "00:03:45-00:03:49 - short censor", 3 * 60 + 45, 3 * 60 + 49);
	expect_range("hms unpadded", "1:02:16-1:03:01", 1 * 3600 + 2 * 60 + 16, 1 * 3600 + 3 * 60 + 1);
	expect_range("reversed", "01:28:19-01:26:21", 1 * 3600 + 26 * 60 + 21, 1 * 3600 + 28 * 60 + 19);
	expect_range("mixed mmss/hms", "1:02:16 - 65:00", 1 * 3600 + 2 * 60 + 16, 65 * 60);

	expect_range("en-dash", std::string("30:20") + " \xE2\x80\x93 " + "30:27", 30 * 60 + 20, 30 * 60 + 27);
	expect_range("em-dash", std::string("30:20") + " \xE2\x80\x94 " + "30:27", 30 * 60 + 20, 30 * 60 + 27);
	expect_range("minus", std::string("30:20") + " \xE2\x88\x92 " + "30:27", 30 * 60 + 20, 30 * 60 + 27);
	expect_range("nbsp", std::string("30:20") + "\xC2\xA0-\xC2\xA0" + "30:27", 30 * 60 + 20, 30 * 60 + 27);

	const std::string screenshot = "30:20 - 30:27 - грудь\n"
				       "\n"
				       "Больше таймингов здесь: https://timings.rte.net.ru/\n"
				       "\n"
				       "Больше таймингов здесь: https://timings.rte.net.ru/\n";
	expect_count("screenshot dump", screenshot, 1);

	expect_count("empty", "", 0);
	expect_count("notes only", "Больше таймингов здесь: https://timings.rte.net.ru/", 0);
	expect_count("two ranges", "00:03:45-00:03:49\n11:25 - 11:28", 2);
	expect_count("url is not a range", "http://localhost:13579/variables.html", 0);

	if (g_failed) {
		std::printf("\n%d test(s) failed\n", g_failed);
		return 1;
	}
	std::printf("\nall tests passed\n");
	return 0;
}
