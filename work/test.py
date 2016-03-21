import subprocess
import sys
import serial
import time
import math
import numpy

#calculate mean
def mean(values):
	if(len(values)) == 0:
		return None
	return sum(values, 0.0) / len(values)

def standardDeviation(values, option):
	if(len(values)) <2:
		return None
	sd = 0.0
	sum = 0.0
	meanValue = mean(values)

	for i in range(0, len(values)):
		diff = values[i] - meanValue
		sum += diff * diff

	sd = math.sqrt(sum/(len(values) - option))
	return sd

if len(sys.argv) != 4:
	print "usage : python iostat.py dev_name save_file_name save_file_name2"
	exit()

dev = sys.argv[1]


list_ary = []
for i in range(0,19):
	list = []
	list_ary.append(list)


svctm_ary = []

def dataSave(_str):

	#data parse
	ary = _str.split('\n')
	for _str in ary:
		if _str == '':
			ary.remove('')
	row1 = ary[1].split(' ')
	row2 = ary[3].split(' ')


	for i in range(0,row1.count('')):
		row1.remove('')

	for i in range(0, row2.count('')):
		row2.remove('')
	del row2[0]
	
	#print(row2[5]);
	#print(row2[11]);
	#svctm_ary.append(float(row2[11]))
	#print(svctm_ary)
	row3 = row2[5].split('.')
	
	file = open(sys.argv[2], 'w')
#	file.write(row2[5])
	#file.write(row3[0] + '\0')
	#file.write(row3[0]+ ' ' + str(standardDeviation(svctm_ary, 1)) + '\0')
	file.write(row3[0] + ' ' + row2[11] + '\0')

	file2 = open(sys.argv[3], 'a')
	file2.write(row3[0] + '\n')
	file2.close()

command = 'iostat ' + dev + ' -x 1'
p = subprocess.Popen(command, shell=True, bufsize=64, stdin=subprocess.PIPE, stderr=subprocess.PIPE, stdout=subprocess.PIPE)

count = -1
string = ''
start = 0
while p.poll() is None:
	l = p.stdout.readline()
	if start == 1:
		string += l
	if l == "\n":
		count += 1
		if count == 2 and start == 1:
			count = 0
			dataSave(string)
			string = ''
		else:
			start = 1
			

