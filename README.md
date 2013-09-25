Nginx gzip handler for static files with automatic precompression
================================

This module allows compession of static files before sending them to client. When processing request of some static file this module will look for a file with name of original file with addition of ".gz" to the end, e.g. when requesting file main.css this module will look for file main.css.gz. If such file exists then module will send it back to client with "Content-Encoding: deflate" header. If such file does not exist then module will open original file, compress its content with best compression level, save it into the ".gz" file and send it back to client.

----------------------------------

TODO:
* Take into consideration either file's content or file's last modification time so that if original file modifies old cache file would not be used.
* Add a config option for module to specify folder in which to store cache files. If not provided use original file's containing folder.
* Add a config option for module to specify compression level, default is 9 (best)
* Do not try to compress original files that are already end with ".gz", send them as is instead (providing according "Content-Encoding" header)