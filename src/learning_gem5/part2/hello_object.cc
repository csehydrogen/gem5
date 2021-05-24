#include "learning_gem5/part2/hello_object.hh"

#include <iostream>

HelloObject::HelloObject(const HelloObjectParams &params) : SimObject(params) {
  std::cout << "Hello World! From a SimObject!" << std::endl;
}