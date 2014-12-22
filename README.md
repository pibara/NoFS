NoFS
====

This user-space fuse based file-system provides a kind of loop back for a raw disk image that makes it 
paricularily suitable for use in the reverse engineering of file-system behaviour by operating systems
and potentially for the analysis of some forms of malware.

If you use NoFs to mount a DD disk image for the first time, an exact copy of the DD file will be shown
by the pseudo file-system. This dd image can than be used in a virtual machine like VirtualBox. 
The interesting behaviour of NoFs starts when the virtual dd image is updated by the OS running in the VM.
Rather than writing the data to the original DD, a set of extra files is created.

* A newdata file containing all data written to the disk image in the order that it was written.
* An event file containing the offsets where the guest-OS indicated the data should be written. Each 64 bit ofset in this
  file holds the ofset in the dd file corresponding to the 512 byte block found at the same position in th newdata file.
* An index file containing a list of offsets into the newdata file, or 0xFFFFFFFFFFFFFFFF if a 512 byte block remains unchanged
  from the original DD image. 

The event file can easily be dumped to a usefull format using a command like:

    od -t d8 -w8 test.dd.event

Every single byte that the guest-OS ever commited to disk is written to the newdata and the event file in a maner that can be used
for thorough analysis. This can be usefull for multiple purposes:

* Security evaluation of cryptographic systems (is no sensitive data ever written to disk in temporary files?).
* Reverse engineering of file-systems and OS-file-system behaviour.
* Malware analysis.

This software was written with the goal of testing an hypothesis regarding a possible subject for my upcomming MSc dissertation for my study at the UCD.
 
