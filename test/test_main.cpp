#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdlib>

int main(int argc, char** argv)
{
	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();

	// JVM-related globals can double-free during static destruction on Linux.
	// Exit immediately after doctest completes to avoid teardown-time aborts.
	std::_Exit(result);
}
