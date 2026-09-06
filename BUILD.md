A simple build script called `maker` is included that can be invoked on  
the command to install libraries and compile ActualMIDIsynth on Linux  
and MacOS.

The script is still in development and in a state of flux, but you  
should always be able to build (providing you have installed your  
platform's development tools, and the ActualMIDIsynth source) by typing  
`./maker -install bass -install bassmidi -build`  
The resulting binary will be placed in the ./mac or ./linux directory

Stating the binary on MacOS will require setting the DYLD_LIBRARY_PATH  
to include the BASS and BASSMIDI libraries, as (at the time of writing)  
there is no mechanism to install them properly.

example:  
`DYLD_LIBRARY_PATH=./mac/bassmidi24-osx/libs:./mac/bass24/libs`
