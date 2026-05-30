# userspace/apps/Makefile - Build userspace applications

.PHONY: all terminal taskmanager files clean

all: terminal taskmanager files

terminal:
	$(MAKE) -C terminal/

taskmanager:
	$(MAKE) -C taskmanager/

files:
	$(MAKE) -C files/

clean:
	$(MAKE) -C terminal/ clean
	$(MAKE) -C taskmanager/ clean
	$(MAKE) -C files/ clean
