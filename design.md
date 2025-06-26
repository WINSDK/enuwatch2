# Design

### Message stream

* Communication over UDP (with retransmit)
* Encrypted using pub-key crypto
* Data stream is compressed (gzip)

### Encryption

* Uses ssh to establish tunnel
* On startup, forks an ssh process `ssh user@host enuwatch --server`.
* Server keeps cache encrypted using chacha20-poly1305
* https://ocaml.org/p/camlzip/latest

### Hashing

* Rolling hash: hmac-sha256
 - <https://news.ycombinator.com/item?id=18243489>
 - <https://beeznest.wordpress.com/2005/02/03/rsyncable-gzip>
 - <https://en.wikipedia.org/wiki/Rolling_hash#Content-based_slicing_using_a_rolling_hash>

## File watching

### Linux

* Uses [Inotify](https://man7.org/linux/man-pages/man7/inotify.7.html)
