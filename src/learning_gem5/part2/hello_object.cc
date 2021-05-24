#include "learning_gem5/part2/hello_object.hh"

#include "base/trace.hh"
#include "debug/Hello.hh"

HelloObject::HelloObject(const HelloObjectParams &params) : SimObject(params) {
  DPRINTF(Hello, "Created the hello object\n");
}