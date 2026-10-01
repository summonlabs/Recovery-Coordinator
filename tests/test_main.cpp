#include "test.hpp"

#include <algorithm>

namespace rectest {

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

void Registry::add(std::string name, std::function<void()> body) {
    cases_.push_back(TestCase{std::move(name), std::move(body)});
}

void require(bool condition, const char* text, const char* file, int line) {
    if (!condition) {
        std::ostringstream out;
        out << file << ":" << line << ": requirement failed: " << text;
        throw Failure(out.str());
    }
}

void require_message(bool condition, std::string message, const char* file, int line) {
    if (!condition) {
        std::ostringstream out;
        out << file << ":" << line << ": " << message;
        throw Failure(out.str());
    }
}

int run_all(const char* suiteName) {
    Registry& registry = Registry::instance();
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::vector<std::string> failures;

    for (const TestCase& test : registry.cases()) {
        if (!registry.filter().empty() && test.name.find(registry.filter()) == std::string::npos) {
            continue;
        }
        std::cout << "[ RUN  ] " << test.name << std::endl;
        try {
            test.body();
            ++passed;
            std::cout << "[  OK  ] " << test.name << std::endl;
        } catch (const Failure& failure) {
            ++failed;
            failures.push_back(test.name + ": " + failure.message());
            std::cout << "[ FAIL ] " << test.name << "\n" << failure.message() << std::endl;
        }
    }

    std::cout << suiteName << ": " << passed << " passed, " << failed << " failed" << std::endl;
    for (const std::string& failure : failures) {
        std::cout << "  FAILED " << failure << std::endl;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace rectest

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument{argv[i]};
        if (argument == "--filter" && i + 1 < argc) {
            ::rectest::Registry::instance().set_filter(argv[++i]);
        }
    }
    return ::rectest::run_all("recovery-tests");
}
