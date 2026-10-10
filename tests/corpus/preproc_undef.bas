'@fbc:pass
#define LIGHT 1
#ifdef LIGHT
print "on"
#else
print "off"
#endif
#undef LIGHT
#ifndef LIGHT
print "off again"
#else
print "on again"
#endif