#!/usr/bin/python
#
#
# Copyright (C) 2013-2014 Canonical
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
import sys, os, json

if len(sys.argv) != 2:
        sys.stderr.write("Usage: " + sys.argv[0] + " jsonfile\n")
        os._exit(1)

file = sys.argv[1]

try:
	f = open(file, 'r')
	data = json.load(f)
	f.close()

	if "power-calibrate" in data:
		print "1% CPU Load:"
		print "  Watts: " + str(data["power-calibrate"]["cpu-load"]["one-percent-cpu-load"])
		print "  R^2:   " + str(data["power-calibrate"]["cpu-load"]["r-squared"])
	if "power-calibrate" in data:
		print "1 Context switch:"
		print "  Watts: " + str(data["power-calibrate"]["context-switches"]["one-context-switch"])
		print "  R^2:   " + str(data["power-calibrate"]["context-switches"]["r-squared"])
	print "Tested:"
	print "  " + data["power-calibrate"]["test-run"]["date"] + " " + data["power-calibrate"]["test-run"]["time"]
	print "  " + data["power-calibrate"]["test-run"]["sysname"] + " " + \
		data["power-calibrate"]["test-run"]["nodename"] + " " + \
		data["power-calibrate"]["test-run"]["release"] + " " + \
		data["power-calibrate"]["test-run"]["machine"]

except:	
	sys.stderr.write("Failed to open JSON file " + file +  "\n");
	os._exit(1)
