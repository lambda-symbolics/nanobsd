#include <sys/ioctl.h>
#include <prop/proplib.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <string.h>
#include <errno.h>
#include <dev/hdaudio/hdaudioio.h>
/* hdaverb [-f /dev/hdaudio0] codecid nid verb param  -> prints result in hex.  Driver-encoded verb:
   12-bit verbs as 0xF05 with 8-bit param, 4-bit verbs as 0x300 with 16-bit param. */
int main(int argc, char **argv) {
  const char *dev = "/dev/hdaudio0";
  if (argc > 2 && strcmp(argv[1], "-f") == 0) { dev = argv[2]; argv += 2; argc -= 2; }
  if (argc != 5) { fprintf(stderr, "usage: hdaverb [-f dev] codecid nid verb param\n"); return 2; }
  int fd = open(dev, O_RDWR); if (fd < 0) err(1, "%s", dev);
  prop_dictionary_t req = prop_dictionary_create(), resp = NULL;
  prop_dictionary_set_int16(req, "codecid", (int16_t)strtol(argv[1], 0, 0));
  prop_dictionary_set_int16(req, "nid", (int16_t)strtol(argv[2], 0, 0));
  prop_dictionary_set_uint32(req, "verb", (uint32_t)strtoul(argv[3], 0, 0));
  prop_dictionary_set_uint32(req, "param", (uint32_t)strtoul(argv[4], 0, 0));
  int e = prop_dictionary_sendrecv_ioctl(req, fd, HDAUDIO_FGRP_COMMAND, &resp);
  if (e) { errno = e; err(1, "ioctl"); }
  uint32_t r = 0; prop_dictionary_get_uint32(resp, "result", &r);
  printf("%08x\n", r); return 0; }
