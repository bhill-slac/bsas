#!../../bin/rhel7-x86_64/bsas

## Register all support components
dbLoadDatabase("../../dbd/bsas.dbd",0,0)
bsas_registerRecordDeviceDriver(pdbbase) 

dbLoadRecords("tx1.db", "P=TX:,FLNK=TX:cnt10")
#dbLoadRecords("big.db", "P=TX:")

iocInit()
