# RepackageUPK

Simple UE3 .UPK repackager. Requires the original .UPK file and the result of its extraction via
 gildor's extract tool (can be obtained at his website: <https://www.gildor.org/downloads>).  
 Will copy the .UPK and replace all the files in it with the ones found in the extracted folder.  
 Cannot add, remove any of the files or change their classes or paths within the package etc.

To use, simply type:

```txt
  ReadUPK ORIGINAL_UPK EXTRACTED_FOLDER NEW_UPK
```

, where:

- **ORIGINAL_UPK** is the path to the original .UPK file that you want to make a copy of,  
- **EXTRACTED_FOLDER** is the path to the folder into which you extracted the contents of the ORIGINAL_UPK with gildor's tool,  
        and which contains the modified files as well,  
- **NEW_UPK** is the path, including the name and the extension, where a new .UPK copy will be created with the modified files.  
        The original .UPK will not be modified.

## Build/run

Only runs on Windows. Should be simple enough to alter to run on Linux.  
To build, open it with Visual Studio and press Build - Build Solution.  
It will say where it created the .exe file in the Output panel.  
You can then run the .exe with supplied arguments as explained above.
