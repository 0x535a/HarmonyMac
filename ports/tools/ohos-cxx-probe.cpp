#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

int main()
{
    std::vector<int> numbers(1000);
    std::iota(numbers.begin(), numbers.end(), 1);
    if (std::accumulate(numbers.begin(), numbers.end(), 0) != 500500) return 2;
    std::mutex lock;
    std::string text;
    std::thread first([&] { std::lock_guard<std::mutex> guard(lock); text += "A"; });
    std::thread second([&] { std::lock_guard<std::mutex> guard(lock); text += "B"; });
    first.join();
    second.join();
    if (text.size() != 2) return 3;
    std::cout << "OHOS_CXX_PROBE_PASS string, vector, iostream, thread, mutex" << std::endl;
    return 0;
}
