#!/bin/bash

get_config() {
	local line

	line=$(grep "^${1}=" include/config/auto.conf)
	if [ -z "$line" ]; then
		return
	fi
	echo "${line#*=}"
}

if [ ! -e include/config/auto.conf ]; then
        echo "Error: auto.conf not generated - run 'make prepare' to create it" >&2
	exit 1
fi

VERSION=$(get_config CONFIG_SUNLIGHT_VERSION)
PATCHLEVEL=$(get_config CONFIG_SUNLIGHT_PATCHLEVEL)
AUXRELEASE=$(get_config CONFIG_SUNLIGHT_AUXRELEASE)
PRODUCT_CODE=$(get_config CONFIG_SUNLIGHT_PRODUCT_CODE)

if [ -z "$VERSION" -o -z "$PATCHLEVEL" -o -z "$AUXRELEASE" ]; then
	# This would be a bug in the Kconfig
	cat <<- END >&2
	ERROR: Missing VERSION, PATCHLEVEL, or AUXRELEASE."
	Please check init/Kconfig.sunlight for correctness.
	END
	exit 1
fi

if [ "$VERSION" = 255 -o "$PATCHLEVEL" = 255 ]; then
	cat <<- END >&2

	ERROR: This release needs to be properly configured.
	Please add real values for SUNLIGHT_VERSION and SUNLIGHT_PATCHLEVEL.

	END
	exit 1
fi


case "$PRODUCT_CODE" in
	1)
		if [ "${PATCHLEVEL}" = "0" ]; then
			SP=""
		else
			SP="${PATCHLEVEL}"
		fi
		SUNLIGHT_PRODUCT_NAME="SUNLIGHT Linux Enterprise ${VERSION}${SP:+ SP}${SP}"
		SUNLIGHT_PRODUCT_SHORTNAME="ENTERPRISE${VERSION}${SP:+-SP}${SP}"
		SUNLIGHT_PRODUCT_FAMILY="ENTERPRISE"
		;;
	2)
		SUNLIGHT_PRODUCT_NAME="openSUNLIGHT Stable ${VERSION}.${PATCHLEVEL}"
		SUNLIGHT_PRODUCT_SHORTNAME="$SUNLIGHT_PRODUCT_NAME"
		SUNLIGHT_PRODUCT_FAMILY="Stable"
		;;
	3)
		SUNLIGHT_PRODUCT_NAME="openSUNLIGHT Rolling"
		SUNLIGHT_PRODUCT_SHORTNAME="$SUNLIGHT_PRODUCT_NAME"
		SUNLIGHT_PRODUCT_FAMILY="Rolling"
		;;
	*)
		echo "Unknown SUNLIGHT_PRODUCT_CODE=${PRODUCT_CODE}" >&2
		exit 1
		;;
esac

SUNLIGHT_PRODUCT_CODE=$(( (${PRODUCT_CODE} << 24) + \
		      (${VERSION} << 16) + (${PATCHLEVEL} << 8) + \
		       ${AUXRELEASE} ))

cat <<END
#ifndef _SUNLIGHT_VERSION_H
#define _SUNLIGHT_VERSION_H

#define SUNLIGHT_PRODUCT_CODE_ENTERPRISE				1
#define SUNLIGHT_PRODUCT_CODE_OPENSUNLIGHT_STABLE			2
#define SUNLIGHT_PRODUCT_CODE_OPENSUNLIGHT_ROLLING		3

#define SUNLIGHT_PRODUCT_FAMILY     "${SUNLIGHT_PRODUCT_FAMILY}"
#define SUNLIGHT_PRODUCT_NAME       "${SUNLIGHT_PRODUCT_NAME}"
#define SUNLIGHT_PRODUCT_SHORTNAME  "${SUNLIGHT_PRODUCT_SHORTNAME}"
#define SUNLIGHT_VERSION            ${VERSION}
#define SUNLIGHT_PATCHLEVEL         ${PATCHLEVEL}
#define SUNLIGHT_AUXRELEASE		${AUXRELEASE}
#define SUNLIGHT_PRODUCT_CODE       ${SUNLIGHT_PRODUCT_CODE}
#define SUNLIGHT_PRODUCT(product, version, patchlevel, auxrelease)		\\
	(((product) << 24) + ((version) << 16) +			\\
	 ((patchlevel) << 8) + (auxrelease))

#endif /* _SUNLIGHT_VERSION_H */
END
