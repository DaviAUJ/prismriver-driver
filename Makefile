obj-m = prismriver_driver.o

all: build clean

build:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules

clean:
	rm -f .*.cmd .*.o *.order *.symvers *.mod* *.o
