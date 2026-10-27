#!/bin/sh
# SPDX-License-Identifier: MIT

#
# This script is responsible for loading firmware blobs from firmware
# partitions on qcom devices. It will make a dir in tmp, mount all of the
# interesting partitions there and then symlink blobs to a single dir that can
# be then provided to the kernel. (At this time only single additional
# directory can be provided)
#
# This script attempts to load everything at runtime and be as generic
# as possible between the target devices: It should allow a single rootfs
# to be used on multiple different devices as long as all the blobs
# are present on dedicated partitions.
# (Usually the case, Samsung devices ship all blobs, other devices may miss
# venus but that still allows for WiFi and modem to work)
#

# Get the slot suffix for A/B devices.
# If qbootctl is available query the active slot using that, otherwise rely on
# the kernel cmdline to contain the slot suffix.
# On non-A/B devices the return value will be empty.
# https://source.android.com/docs/core/architecture/bootloader/updating#slots
ab_get_slot() {
	if command -v qbootctl > /dev/null; then
		ab_slot_suffix=$(qbootctl -a | grep -o 'Active slot: _[ab]' | cut -d ":" -f2 | xargs) || :
	else
		ab_slot_suffix=$(grep -o 'androidboot\.slot_suffix=..' /proc/cmdline |  cut -d "=" -f2) || :
	fi
	echo "$ab_slot_suffix"
}

# Check path $1 for existence and link it to $2
link_path() {
	if [ -e "$1" ]
	then
		mkdir -p "$(dirname "$2")"
		ln -s "$1" "$2"
	fi
}

# Configurations:

# List of partitions to be mounted and inspected for blobs.
FW_PARTITIONS="
	apnhlos
	bluetooth$(ab_get_slot)
	dsp$(ab_get_slot)
	modem$(ab_get_slot)
	persist
	vendor$(ab_get_slot)
	RADIO
"

# List of partitions to mount dynamic partitions from.
SUPER_PARTITIONS="
	super
	system
"

# Base directory to be used to unfold the partitions into.
BASEDIR="/run/msm-firmware-loader"

# Preparations:

# This script is intended to run before udev. This means that writeable fs
# may not be available yet. Since this script only creates symlinks, it
# uses tmpfs to work around the early-run limitations as well as to reduce
# disk wear slightly.
mount -o mode=755,nodev,noexec,nosuid -t tmpfs none "$BASEDIR"

mkdir -p "$BASEDIR/mnt"
mkdir -p "$BASEDIR/target"

# Scanning and mounting partitions we're interested in:

# Modern android devices use dynamic partitions for the system.
# To gather firmware from such partitions, search for a "super"
# or "system" partition, and if one is present, map it and try
# to locate firmware partitions of interest inside.
if command -v make-dynpart-mappings > /dev/null
then
	for part in /sys/block/mmcblk*/mmcblk*p* /sys/block/sd*/sd*
	do
		if ! [ -e "$part" ]; then continue; fi;

		DEVNAME="$(grep DEVNAME "$part"/uevent | sed 's/DEVNAME=//g')"
		PARTNAME="$(grep PARTNAME "$part"/uevent | sed 's/PARTNAME=//g')"

		if [ -z "${SUPER_PARTITIONS##*"$PARTNAME"*}" ] && [ -n "$PARTNAME" ]
		then
			# system is for retrofit targets, not all devices may be
			# retrofit, in which case make-dynpart-mappings will fail.
			if ! make-dynpart-mappings "/dev/$DEVNAME"; then continue; fi;

			for dynpart in /dev/mapper/*
			do
				PARTNAME="$(basename "$dynpart")"
				if [ -z "${FW_PARTITIONS##*"$PARTNAME"*}" ] && [ -n "$PARTNAME" ]
				then
					mkdir -p "$BASEDIR/mnt/$PARTNAME"
					mount -o ro,nodev,noexec,nosuid \
						"$dynpart" "$BASEDIR/mnt/$PARTNAME"
				fi
			done

			break
		fi
	done
fi

# /dev/disk/by-partlabel symlinks don't exist yet, scan sysfs for names instead
for part in /sys/block/mmcblk*/mmcblk*p* /sys/block/sd*/sd*
do
	if ! [ -e "$part" ]; then continue; fi;

	DEVNAME="$(grep DEVNAME "$part"/uevent | sed 's/DEVNAME=//g')"
	PARTNAME="$(grep PARTNAME "$part"/uevent | sed 's/PARTNAME=//g')"

	if [ -z "${FW_PARTITIONS##*"$PARTNAME"*}" ] && [ -n "$PARTNAME" ] && [ ! -d "$BASEDIR/mnt/$PARTNAME" ]
	then
		mkdir -p "$BASEDIR/mnt/$PARTNAME"
		mount -o ro,nodev,noexec,nosuid \
			"/dev/$DEVNAME" "$BASEDIR/mnt/$PARTNAME"
		# Some devices have 600 permissions on persist, therefore we need an additional
		# mount with UID and GID mappings for hexagonrpcd to access it
		sensors_file="$BASEDIR/mnt/persist/sensors/registry/registry/sensors_registry"
		if [ "$PARTNAME" = "persist" ] && [ -f "$sensors_file" ]
		then
			source_uid="$(stat -c '%u' "$sensors_file")"
			source_gid="$(stat -c '%g' "$sensors_file")"
			target_uid="$(id -u fastrpc)"
			target_gid="$(id -g fastrpc)"
			mkdir -p "$BASEDIR/mnt/persist-fastrpc"
			# Kernel requires both UID and GID mapped
			mount -o ro,nodev,noexec,nosuid \
				--map-users="$source_uid:$target_uid:1" \
				--map-groups="$source_gid:$target_gid:1" \
				"/dev/$DEVNAME" "$BASEDIR/mnt/persist-fastrpc"
		fi
	fi
done

# Linking blobs from all partitions:

# Backup the preselected path, link all of the installed blobs.
# This is needed for devices that require blobs either not present
# on the partitions (e.g. venus on many msm8916 devices) or if
# the device has secure-boot disabled and can run newer blobs.
EXTRA_PATH="$(cat /sys/module/firmware_class/parameters/path)"

if [ -d "$EXTRA_PATH" ]
then
	for blob in "$EXTRA_PATH"/*
	do
		if ! [ -e "$blob" ]; then break; fi
		ln -s "$blob" "$BASEDIR/target/$(basename "$blob")"
	done
fi

# Scan through mounted partitions and symlink all of the blobs.
# This loop ignores blobs with names already present in the
# target to allow preinstalled blobs to override ones in the partitions.
for blob in "$BASEDIR"/mnt/*/image/* "$BASEDIR"/mnt/*/firmware/*
do
	if ! [ -e "$blob" ]; then continue; fi;

	DIR="$(dirname "$blob")"
	BLOBBASE="${blob##*/}"
	BLOBBASE="${BLOBBASE%.*}"

	# Skip blob prefix if it's already present.
	for prefix in "$BASEDIR/target/$BLOBBASE."*
	do
		if [ -e "$prefix" ]; then continue 2; fi
	done

	for part in "$DIR"/"$BLOBBASE"*
	do
		if [ -f "$part" ]
		then
			ln -s "$part" "$BASEDIR/target/$(basename "$part")"
		fi
	done
done

# Check for sns.reg in persist partition
link_path "$BASEDIR"/mnt/persist/sensors/sns.reg "$BASEDIR"/target/qcom/sensors/sns.reg

# Mount hexagonrpc paths, see
# https://github.com/linux-msm/hexagonrpc#hexagonfs

link_path "$BASEDIR/mnt/vendor$(ab_get_slot)/etc/acdbdata" "$BASEDIR/hexagonrpc/acdb"

if [ -d "$BASEDIR/mnt/dsp$(ab_get_slot)/adsp" ] || \
	[ -d "$BASEDIR/mnt/dsp$(ab_get_slot)/cdsp" ] || \
	[ -d "$BASEDIR/mnt/dsp$(ab_get_slot)/sdsp" ]
then
	# Some devices with separated dsp_a/dsp_b
	link_path "$BASEDIR/mnt/dsp$(ab_get_slot)" "$BASEDIR/hexagonrpc/dsp"
else
	# Documented hexagonrpc path
	link_path "$BASEDIR/mnt/vendor$(ab_get_slot)/dsp" "$BASEDIR/hexagonrpc/dsp"
fi

link_path "$BASEDIR/mnt/vendor$(ab_get_slot)/etc/sensors/config" "$BASEDIR/hexagonrpc/sensors/config"
link_path "$BASEDIR/mnt/persist-fastrpc/sensors/registry/registry" "$BASEDIR/hexagonrpc/sensors/registry"
link_path "$BASEDIR/mnt/vendor$(ab_get_slot)/etc/sensors/sns_reg_config" "$BASEDIR/hexagonrpc/sensors/sns_reg.conf"
# hexagonrpc expects socinfo packaged from downstream here, but because it is
# runtime-specific, the closest we have is socinfo from runtime kernel.
link_path "/sys/devices/soc0" "$BASEDIR/hexagonrpc/socinfo"

# Check WCNSS_qcom_wlan_nv.bin in persist partition
link_path "$BASEDIR"/mnt/persist/WCNSS_qcom_wlan_nv.bin "$BASEDIR"/target/WCNSS_qcom_wlan_nv.bin

# Fixup the directory structure:

# venus (video encoder/decoder) blobs are expected to be in a subdir.
# Re-link the blobs if the venus firmware wasn't already preinstalled.
# Different platforms expect firmware in different subdirs
# (as in linux-firmware-qcom) so the venus dir is duplicated multiple times
# under possible names for the script to be generic without complex detection.

if [ -f "$BASEDIR/target/venus.mdt" ] && ! [ -d "$BASEDIR/target/qcom/venus-x" ]
then
	mkdir -p "$BASEDIR/target/qcom/venus-x"
	for part in "$BASEDIR"/target/venus.*
	do
		ln -s "$part" "$BASEDIR/target/qcom/venus-x/$(basename "$part")"
	done
fi

VENUS_DIRS="
	venus-1.8
	venus-3.0
	venus-4.2
	venus-4.4
	venus-5.2
	venus-5.4
	vpu-1.0
	vpu-2.0
"

for vdir in $VENUS_DIRS
do
	if ! [ -d "$BASEDIR/target/qcom/$vdir" ] && [ -f "$BASEDIR/target/venus.mdt" ]
	then
		ln -s "$BASEDIR/target/qcom/venus-x" \
			"$BASEDIR/target/qcom/$vdir"
	fi
done

# WCNSS_qcom_wlan_nv.bin needs to be relocated too
if [ -h "$BASEDIR"/target/WCNSS_qcom_wlan_nv.bin ]
then
	if ! [ -f "$BASEDIR"/target/wlan/prima/WCNSS_qcom_wlan_nv.bin ]
	then
		link_path "$BASEDIR"/target/WCNSS_qcom_wlan_nv.bin "$BASEDIR"/target/wlan/prima/WCNSS_qcom_wlan_nv.bin
	fi
fi

# Devices with ath10k wcn3990 wifi/bt have bluetooth firmware on a separate
# "bluetooth" partition. Files from it need to be placed into qca/ subdir.
# Check if bluetooth partition was mounted, and if so, link files into qca/
if [ -d "$BASEDIR/mnt/bluetooth$(ab_get_slot)" ]
then
	for btblob in "$BASEDIR/mnt/bluetooth$(ab_get_slot)/image"/*
	do
		link_path "$btblob" "$BASEDIR"/target/qca/"$(basename "$btblob")"
	done
fi

# Some kernel versions expect .mbn instead of legacy .mdt
# Symlink these files together, the kernel can autodetect the type.
find "$BASEDIR"/target/ \
	-name '*.mdt' \
	-exec sh -c 'ln -s $0 ${0%.mdt}.mbn' {} \;

# It's customary to define device-model-specific firmware prefix in
# devicetree to separate similarly named but differently signed blobs
# between different devices. This script handles those by guessing
# the common prefix and symlinking the firmware tree to it, which
# relies on following implications:
#  - The device uses the same firmware prefix for all blobs;
#  - All blob file names in the prefix are same as in downstream,
#    with the exception that .mbn instead of legacy .mdt may be used
FIRMWARE_PREFIX=$(find /sys/firmware/devicetree -name "firmware-name" | head -n1 | xargs cat | xargs dirname)

if [ -n "$FIRMWARE_PREFIX" ]
then
	link_path "$BASEDIR/target" "$BASEDIR/target/$FIRMWARE_PREFIX"
fi

# Set the new custom firmware path:
printf "%s" "$BASEDIR/target" > /sys/module/firmware_class/parameters/path

