# octane
C++11 memory accelerator. Simply add octane.cpp to your projects to increase performance of new/delete (array versions too). Standard memory manager considerations apply when using shared libraries. If the shared library deletes memory allocated by a call to new then it will also need octane.cpp when compiling otherwise it will not work.

Status: Stable.
Version: 0.2
License: GPL v3
