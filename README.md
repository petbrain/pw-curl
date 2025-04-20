# PetWay-based CURL wrapper

This is a C version of my original Python code.

It was too painful to make progress with PyCurl.
It's a long story, but here is the result.

Original python code was 410 lines long.
This C version has 375 without header file.
Not a big difference, but without PetWay library
it would be thousands of lines, I presume.
Not mentioning spooky amount of bugs and other fleas.

[fetch.c](fetch.c) is an example how to use this wrapper
in particular and PetWay library in general.
It's a simple downloader that can fetch URLs in parallel.

[pw_http_util.c](pw_http_util.c) contains header parsing
and other helper routines.
