#!/bin/sh
# =============================================================================
# /etc/udhcpc.sh — udhcpc lease handler for Beta SoC QEMU net regression
#
# udhcpc calls this script with $1 = {bound, renew, deconfig, leasefail}.
# Environment variables set by udhcpc:
#   $interface  — network interface (eth0)
#   $ip         — assigned IP address
#   $subnet     — subnet mask
#   $router     — default gateway IP
#   $dns        — space-separated list of DNS servers
#
# Ref: https://udhcp.busybox.net/README.udhcpc
# =============================================================================

case "$1" in
    bound|renew)
        echo "[udhcpc] $1 on $interface: ip=$ip subnet=${subnet:-/24} gw=$router"

        # Flush existing addresses on this interface
        ip addr flush dev "$interface" 2>/dev/null

        # Set IP address (convert dotted-decimal mask to CIDR if needed)
        if [ -n "$subnet" ]; then
            # Convert mask to prefix length using awk
            PREFIX=$(printf '%d' "$(echo "$subnet" | awk -F. '{
                split($0, a, ".");
                mask = 0;
                for (i=1; i<=4; i++) {
                    v = a[i]+0;
                    for (b=7; b>=0; b--) {
                        if (int(v/2^b) % 2 == 1) mask++;
                        else break;
                    }
                }
                print mask
            }')")
            ip addr add "${ip}/${PREFIX}" dev "$interface" 2>/dev/null
        else
            ip addr add "${ip}/24" dev "$interface" 2>/dev/null
        fi

        # Default route
        if [ -n "$router" ]; then
            ip route add default via "$router" dev "$interface" 2>/dev/null
        fi

        # DNS (write to resolv.conf if writable)
        if [ -n "$dns" ] && [ -w /etc/resolv.conf ]; then
            : > /etc/resolv.conf
            for d in $dns; do
                echo "nameserver $d" >> /etc/resolv.conf
            done
        fi

        echo "[udhcpc] configuration applied: $(ip addr show "$interface" | grep inet)"
        ;;

    deconfig)
        ip addr flush dev "$interface" 2>/dev/null
        ip route del default dev "$interface" 2>/dev/null
        echo "[udhcpc] $interface deconfigured"
        ;;

    leasefail|nak)
        echo "[udhcpc] lease failed on $interface ($1)"
        exit 1
        ;;
esac

exit 0
