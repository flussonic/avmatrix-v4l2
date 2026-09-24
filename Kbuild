# The kbuild side of the module; the Makefile next to it is what a person
# (or dkms) runs.
ccflags-y   += -I$(src)/hwsv4l2 -I$(src)/include -Wall \
               -DHWS_VERSION=\"$(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' $(src)/dkms.conf)\"

OBJS        := hwsv4l2_module hwsv4l2_hw hwsv4l2_timings hwsv4l2_video \
               hwsv4l2_audio hwsv4l2_sysfs

obj-m       := hwsv4l2.o
hwsv4l2-y   := $(addsuffix .o,$(addprefix hwsv4l2/,$(OBJS)))
