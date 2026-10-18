obj-m += acx-mac80211.o
acx-mac80211-y := pci.o usb.o common.o merge.o debug.o cmd.o ie.o init.o \
	utils.o cardsetting.o tx.o rx.o main.o boot.o
ccflags-y := -DCONFIG_ACX_MAC80211=1 -DCONFIG_ACX_MAC80211_USB=1 \
	-DCONFIG_ACX_MAC80211_PCI=1 -DACX_GIT_VERSION=\"port-7.2\"
