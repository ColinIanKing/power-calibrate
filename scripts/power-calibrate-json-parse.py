#!/usr/bin/python
#
# Copyright (C) 2014 Canonical
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
# This script takes json output from power-calibrate and health-check
# to calculate an esimate of power consumption 
#
#
import sys, os, json

def r2tostr(r2):
	if r2 < 0.4:
		return "very poor"
	if r2 < 0.75:
		return "poor"
	if r2 < 0.80:
		return "fair"
	if r2 < 0.90:
		return "good"
	if r2 < 0.95:
		return "very good"
	if r2 < 1.0:
		return "excellent"
	return "perfect"

if len(sys.argv) != 3:
        sys.stderr.write("Usage: " + sys.argv[0] + " power-calibrate.json health-check.json\n")
        os._exit(1)

try:
	file = sys.argv[1]
	f = open(file, 'r')
	data = json.load(f)
	f.close()
except:	
	sys.stderr.write("Failed to open and parse JSON file " + file +  "\n");
	os._exit(1)

if not "power-calibrate" in data:
	sys.stderr.write("Failed to find power calibration data in file " + file +  "\n");
	os._exit(1)

pc = data["power-calibrate"]
if "cpu-load" in pc:
	cpu_1pc = pc["cpu-load"]["one-percent-cpu-load"]
	cpu_r2 = pc["cpu-load"]["r-squared"]
else:
	cpu_1pc = 0
	cpu_r2 = 0

if "context-switches" in pc:
	ctxt_sw = pc["context-switches"]["one-context-switch"]
	ctxt_r2 = pc["context-switches"]["r-squared"]
else:
	ctxt_sw = 0
	ctxt_r2 = 0

if not "test-run" in pc:
	sys.stderr.write("Failed to find power calibration machine information in file " + file +  "\n");
	os._exit(1)

test_date = pc["test-run"]["date"]
test_time = pc["test-run"]["time"]
test_sys  = pc["test-run"]["sysname"]
test_node = pc["test-run"]["nodename"]
test_rel  = pc["test-run"]["release"]
test_arch = pc["test-run"]["machine"]

try:
	file = sys.argv[2]
	f = open(file, 'r')
	data = json.load(f)
	f.close()
except:
	sys.stderr.write("Failed to open and parse JSON file " + file +  "\n");
	os._exit(1)

if not "health-check" in data:
	sys.stderr.write("Failed to find health-check data in file " + file +  "\n");
	os._exit(1)

hc = data["health-check"]
if not "cpu-load" in hc:
	sys.stderr.write("Failed to find cpu-load stats in health-check file " + file +  "\n");
	os._exit(1)

total_cpu = hc["cpu-load"]["cpu-load-total"]["total-cpu-percent"]

if not "context-switches" in hc:
	sys.stderr.write("Failed to find context-switch stats in health-check file " + file +  "\n");
	os._exit(1)

ctxt_switches = hc["context-switches"]["context-switches-total"]["context-switch-total-rate"]

cpu_total_watts = cpu_1pc * total_cpu
ctxt_total_watts = ctxt_sw * ctxt_switches
total_watts = cpu_total_watts + ctxt_total_watts

if total_watts == 0.0:
	print "CPU               : %8.3f Watts (%.2f%% CPU)" % \
		(cpu_total_watts, total_cpu)
	print "Context Switches  : %8.3f Watts (%.2f switches)" % \
		(ctxt_sw * ctxt_switches, ctxt_switches)
	print "Total             : %8.3f Watts" % total_watts
else:
	rel = ((cpu_total_watts * cpu_r2) + (ctxt_total_watts * ctxt_r2)) / total_watts
	cpu_pc = cpu_total_watts / total_watts * 100
	ctxt_pc = ctxt_total_watts / total_watts * 100
	print "CPU               : %8.3f Watts (%.2f%% CPU, %.2f%% of total power)" % \
		(cpu_total_watts, total_cpu, cpu_pc)
	print "Context Switches  : %8.3f Watts (%.2f switches, %.2f%% of total power)" % \
		(ctxt_sw * ctxt_switches, ctxt_switches, ctxt_pc)
	print "Total             : %8.3f Watts (%s estimate)" % (total_watts, r2tostr(rel))


print " "

print "Note, estimate is based on calibration data:"
print " 1%% CPU           : %10.5f Watts, R^2 %f (%s)" % (cpu_1pc, cpu_r2, r2tostr(cpu_r2))
print " 1 Context Switch : %10.5f Watts, R^2 %f (%s)" % (ctxt_sw, ctxt_r2, r2tostr(ctxt_r2))
print " "

print "Tested on " + test_date + " at " + test_time + " on " + test_node + " (" + \
	test_sys + " " + test_rel + " " + test_arch + ")"
