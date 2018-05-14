libcineform
===========

CineForm decoding and encoding library, forked from the [CineForm SDK](https://github.com/gopro/cineform-sdk).
The main goal here is to remove a lot of unsued stuff and keep both the code repository and library sizes contained.
Also, make it build on all platforms and compilers, while allowing a clean integration into third party project.

## Building libcineform

> $ cd libcineform/build/  
> $ cmake ..  
> $ make  

## CineForm Introduction

GoPro CineForm® is a 12-bit, full-frame wavelet compression video codec. It is designed for speed and quality, at the expense of a very high compression size. Image compression is a balance of size, speed and quality, and you can only choose two. CineForm was the first of its type to focus on speed, while supporting higher bit depths for image quality. More recent examples would be Avid DNxHD® and Apple ProRES®, although both divide the image into blocks using DCT. The full frame wavelet as a subject quality advantage over DCTs, so you can compression more without classic ringing or block artifact issues.

Pixel formats supported:
* 8/10/16-bit YUV 4:2:2 compressed as 10-bit, progressive or interlace 
* 8/10/16-bit RGB 4:4:4 compressed at 12-bit progressive
* 8/16-bit RGBA 4:4:4:4 compressed at 12-bit progressive
* 12/16-bit CFA Bayer RAW, log encoded and compressed at 12-bit progressive
* Dual channel stereoscopic/3D in any of the above.

Compression ratio: between 10:1 and 4:1 are typical, greater ranges are possible. CineForm is a constant quality design, bit-rates will vary as needed for the scene. Whereas most other intermediate video codecs are a constant bit-rate design, quality varies depending on the scene.

## License Terms

CineForm-SDK is licensed under either:

* Apache License, Version 2.0, (LICENSE-APACHE or http://www.apache.org/licenses/LICENSE-2.0)
* MIT license (LICENSE-MIT or http://opensource.org/licenses/MIT)

And so is libcineform...

