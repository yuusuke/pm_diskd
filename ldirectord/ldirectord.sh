#!/bin/sh
#
# ldirectord  Linux Director Daemon
#
# chkconfig: 2345 92 40
# description: Start and stop ldirectord on non-heartbeat systems
#              Using the config file /etc/ha.d/ldirectord.cf
#              
# processname: ldirectrod
# config: /etc/ha.d/ldirectord.cf
#
# Author: Horms <horms@vergenet.net>
# Released: April 2000
# Licence: GNU General Public Licence
#

# Source function library.
if
  [ -f /etc/rc.d/init.d/functions ]
then
  . /etc/rc.d/init.d/functions
else
  function action {
    echo "$1"
    shift
    $@
  }
fi

[ -x /usr/sbin/ldirectord ] || exit 0


######################################################################
# Read arument and take action as appropriate
######################################################################

case "$1" in
  start)
        action "Starting ldirectord" /usr/sbin/ldirectord start
	;;
  stop)
        action "Stopping ldirectord" /usr/sbin/ldirectord stop
	;;
  restart)
	$0 stop
	$0 start
	;;
  status)
	/usr/sbin/ldirectord status
	;;
  reload|force-reload)
  	/usr/sbin/ldirectord reload
	;;
  *)
	echo "Usage: ldirectord
	{start|stop|restart|status|reload|force-reload}"
	exit 1
esac

exit 0
