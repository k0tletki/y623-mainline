/* tinyalsa built without plugins: pcm_close() still calls this */
void snd_utils_close_dev_node(void *node) { (void)node; }
