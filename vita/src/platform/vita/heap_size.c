// Texture conversion scratch is 8 MiB; vitaGL also touches libc for FFP shader strings.
unsigned int sceLibcHeapSize = 24u * 1024u * 1024u;
