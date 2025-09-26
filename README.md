# Communication Library for Hedgehog

## Dependencies

- UCX (v1.19.0 or above)
- PMIx

## TODO

- [ ] add utilities to build tags
  - [ ] function that add a tag field with offset and number of bits (probably
        faster to do this by hand though)
  - [ ] ANY_TAG mask
- [ ] try different storages for the buffer cache
- [ ] make sure everything is thread safe (UCX thread mode seems to have
      issues)
- [ ] add active message functions
- [ ] stream???
