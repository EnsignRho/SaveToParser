DECLARE INTEGER		process_save_to_file ;
						IN SaveToParser.dll ;
					STRING tcInputFile, ;
					STRING tcOutputFile

lcTest		= "Hello, world!"
lnValue		= 12345
lfValue		= 123.45678
lnBigNum	= 0 + (9999999999 - 9999999999)
ldDate		= DATE()
ltDateTime	= DATETIME()
SAVE TO test.mem

process_save_to_file("test.mem", "test.out")

lcText = FILETOSTR("test.out")
? lcText
