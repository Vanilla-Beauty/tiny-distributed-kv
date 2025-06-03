#include <cstdlib>
#include <ctime>

int GetRandomElectTimeOut(int min, int max) {
  static bool is_seeded = false;
  if (!is_seeded) {
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    is_seeded = true;
  }
  return min + std::rand() % (max - min + 1);
}