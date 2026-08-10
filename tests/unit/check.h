#pragma once

#include <iostream>
#include <string>

namespace checkdetail {

inline int &Checks()
{
	static int count = 0;
	return count;
}

inline int &Failures()
{
	static int count = 0;
	return count;
}

inline void Fail(const char *file, int line, const std::string &text)
{
	++Failures();
	std::cerr << "FAIL " << file << ":" << line << ": " << text
		  << std::endl;
}

} // namespace checkdetail

#define CHECK(cond)                                                         \
	do {                                                                \
		++checkdetail::Checks();                                    \
		if (!(cond)) {                                               \
			checkdetail::Fail(__FILE__, __LINE__, std::string(   \
							     #cond));         \
		}                                                            \
	} while (0)

#define CHECK_EQ(a, b)                                                      \
	do {                                                                \
		++checkdetail::Checks();                                    \
		if (!((a) == (b))) {                                         \
			checkdetail::Fail(__FILE__, __LINE__,                \
					  std::string(#a " == " #b));        \
		}                                                            \
	} while (0)

#define CHECK_NE(a, b)                                                      \
	do {                                                                \
		++checkdetail::Checks();                                    \
		if (!((a) != (b))) {                                         \
			checkdetail::Fail(__FILE__, __LINE__,                \
					  std::string(#a " != " #b));        \
		}                                                            \
	} while (0)

// Prints the conventional PASS/FAIL summary and returns the process exit code.
inline int FinishTests(const char *label)
{
	const int failed = checkdetail::Failures();
	std::cout << (failed == 0 ? "PASS" : "FAIL") << " [" << label
		  << "]: " << checkdetail::Checks() << " checks, " << failed
		  << " failures" << std::endl;
	return failed == 0 ? 0 : 1;
}

#define RUN_TESTS(label) return FinishTests(label)