

#include <iostream>
#include <Windows.h>
#include <io.h>     // for _open_osfhandle
#include <fcntl.h>  // for _O_RDONLY
#include <string>
#include <vector>
#include <algorithm>
#include "WinError.h"

// On Linux you use std::string for file paths instead of std::wstring
// and the standard C API for reading/writing files instead of the Windows' one
// It's just that the Windows API allows you to read a file with shared access (non-exclusively).
// It's not critical for this program, just would annoying if the file is already opened by some other program.

#define PACKAGE_FILE_TAG			0x9E2A83C1

static bool fileExists(LPCWSTR path) {
	DWORD fileAttribs = GetFileAttributesW(path);
	if (fileAttribs == INVALID_FILE_ATTRIBUTES) {
		return false;
	}
	if ((fileAttribs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
		return false;
	}
	return true;
}

void readString(std::wstring& str, FILE* file) {
	int length;
	fread(&length, 4, 1, file);
	if (length > 0) {
		std::string strSingleByte(length - 1, '\0');
		fread(&strSingleByte.front(), 1, length, file);
		str.reserve(length - 1);
		for (char c : strSingleByte) {
			str.push_back((wchar_t)c);
		}
	} else {
		str.reserve(-length - 1);
		fread(&str.front(), 2, -length - 1, file);
	}
}
struct FlagWithName {
	const char* name = nullptr;
	DWORD value = 0;
};

std::vector<FlagWithName> allPackageFlags;
std::vector<FlagWithName> allCompressionFlags;
std::vector<FlagWithName> allExportFlags;

void addNamedFlag(std::vector<FlagWithName>& ar, const char* name, DWORD value) {
	ar.push_back(FlagWithName{ name, value });
}

struct NameData {
	std::wstring name;
	int numberPart = 0;
};

NameData readNameData(std::vector<std::wstring>& nameMap, FILE* file) {
	NameData newData;
	int nameIndex;
	fread(&nameIndex, 4, 1, file);
	if (nameIndex < 0 || nameIndex >= nameMap.size()) {
		printf("Name index %d outside the range [0;%d)\n", nameIndex, (int)nameMap.size());
		exit(-1);
	}
	newData.name = nameMap[nameIndex];
	fread(&newData.numberPart, 4, 1, file);
	return newData;
}

std::wstring nameDataToString(NameData& nameData) {
	std::wstring result = nameData.name;
	if (nameData.numberPart) {
		result += L'_';
		unsigned __int64 numberPart64 = nameData.numberPart - 1;
		char buffer[25]{ '\0' };
		_ui64toa_s(numberPart64, buffer, sizeof buffer, 10);
		result.reserve(result.size() + strlen(buffer));
		for (char* c = buffer; *c != '\0' && c - buffer <= sizeof buffer; ++c) {
			result += (wchar_t)*c;
		}
	}
	return result;
}

struct UEGuid {
	DWORD a = 0;
	DWORD b = 0;
	DWORD c = 0;
	DWORD d = 0;
};

void readGuid(UEGuid& guid, FILE* file) {
	fread(&guid.a, 4, 1, file);
	fread(&guid.b, 4, 1, file);
	fread(&guid.c, 4, 1, file);
	fread(&guid.d, 4, 1, file);
}

void printGuid(UEGuid& guid) {
	printf("%.8x-%.4x-%.4x-%.2x%.2x-%.2x%.2x%.2x%.2x%.2x%.2x", guid.a, guid.b & 0xffff, (guid.b >> 16) & 0xffff, guid.c & 0xff,
		(guid.c >> 8) & 0xff, (guid.c >> 16) & 0xff, (guid.c >> 24) & 0xff, guid.d & 0xff, (guid.d >> 8) & 0xff,
		(guid.d >> 16) & 0xff, (guid.d >> 24) & 0xff);
}

void printFlags(DWORD flagField, std::vector<FlagWithName>& ar, const char* spaces = nullptr) {
	printf("[");
	bool isFirst = true;
	for (FlagWithName& fwn : ar) {
		if ((flagField & fwn.value) != 0) {
			if (!isFirst) {
				printf(",\n");
			} else {
				printf("\n");
			}
			if (spaces) printf("%s", spaces);
			printf("  %s", fwn.name);
			isFirst = false;
		}
	}
	if (!isFirst) {
		printf("\n");
		if (spaces) printf("%s", spaces);
	}
	printf("]");
}

void printHelp() {
	printf("%s\n",
	"Simple UE3 .UPK repackager. Requires the original .UPK file and the result of its extraction via"
	" gildor's extract tool (can be obtained at his website: https://www.gildor.org/downloads)."
	" Will copy the .UPK and replace all the files in it with the ones found in the extracted folder."
	" Cannot add, remove any of the files or change their classes or paths within the package etc.\n\n"
	"To use, simply type:\n"
	"  RepackageUPK ORIGINAL_UPK EXTRACTED_FOLDER NEW_UPK\n"
	", where:\n"
	"  ORIGINAL_UPK is the path to the original .UPK file that you want to make a copy of,\n"
	"  EXTRACTED_FOLDER is the path to the folder into which you extracted the contents of the ORIGINAL_UPK with gildor's tool,\n"
	"      and which contains the modified files as well,\n"
	"  NEW_UPK is the path, including the name and the extension, where a new .UPK copy will be created with the modified files.\n"
	"      The original .UPK will not be modified."
	);
}

int wmain(int argc, wchar_t** argv)
{
	struct CloseFilesAtTheEnd {
	public:
		~CloseFilesAtTheEnd() {
			if (file) fclose(file);
			if (writeHandle) CloseHandle(writeHandle);
		}
		FILE* file = nullptr;
		HANDLE writeHandle = NULL;
	} closeFilesAtTheEnd;

	if (argc != 4) {
		printHelp();
		return -1;
	}

	addNamedFlag(allPackageFlags, "AllowDownload", 0x00000001);
	addNamedFlag(allPackageFlags, "ClientOptional", 0x00000002);
	addNamedFlag(allPackageFlags, "ServerSideOnly", 0x00000004);
	addNamedFlag(allPackageFlags, "Cooked", 0x00000008);
	addNamedFlag(allPackageFlags, "Unsecure", 0x00000010);
	addNamedFlag(allPackageFlags, "SavedWithNewerVersion", 0x00000020);
	addNamedFlag(allPackageFlags, "Need", 0x00008000);
	addNamedFlag(allPackageFlags, "Compiling", 0x00010000);
	addNamedFlag(allPackageFlags, "ContainsMap", 0x00020000);
	addNamedFlag(allPackageFlags, "Trash", 0x00040000);
	addNamedFlag(allPackageFlags, "DisallowLazyLoading", 0x00080000);
	addNamedFlag(allPackageFlags, "PlayInEditor", 0x00100000);
	addNamedFlag(allPackageFlags, "ContainsScript", 0x00200000);
	addNamedFlag(allPackageFlags, "ContainsDebugInfo", 0x00400000);
	addNamedFlag(allPackageFlags, "RequireImportsAlreadyLoaded", 0x00800000);
	addNamedFlag(allPackageFlags, "StoreCompressed", 0x02000000);
	addNamedFlag(allPackageFlags, "StoreFullyCompressed", 0x04000000);
	addNamedFlag(allPackageFlags, "ContainsFaceFXData", 0x10000000);
	addNamedFlag(allPackageFlags, "NoExportAllowed", 0x20000000);
	addNamedFlag(allPackageFlags, "StrippedSource", 0x40000000);
	addNamedFlag(allPackageFlags, "FilterEditorOnly", 0x80000000);

	addNamedFlag(allCompressionFlags, "ZLIB", 0x01);
	addNamedFlag(allCompressionFlags, "LZO", 0x02);
	addNamedFlag(allCompressionFlags, "LZX", 0x04);
	addNamedFlag(allCompressionFlags, "BiasMemory", 0x10);
	addNamedFlag(allCompressionFlags, "BiasSpeed", 0x20);
	addNamedFlag(allCompressionFlags, "ForcePPUDecompressZLib", 0x80);

	addNamedFlag(allExportFlags, "ForcedExport", 0x1);
	addNamedFlag(allExportFlags, "ScriptPatcherExport", 0x2);
	addNamedFlag(allExportFlags, "MemberFieldPatchPending", 0x4);

	HANDLE fileHandle = CreateFileW(
		argv[1],
		GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fileHandle == INVALID_HANDLE_VALUE) {
		std::cout << "Failed to open file\n";
		return -1;
	}
	closeFilesAtTheEnd.writeHandle = fileHandle;
	DWORD bytesWritten = 0;
	const wchar_t* writeFileName = argv[3];
	HANDLE writeHandle = CreateFileW(writeFileName,
		GENERIC_WRITE, NULL, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (writeHandle == INVALID_HANDLE_VALUE) {
		WinError err;
		std::cout << "Failed to create file at location: ";
		std::wcout << writeFileName << L'\n' << err.getMessage() << L'\n';
		return -1;
	}
	closeFilesAtTheEnd.writeHandle = writeHandle;
	int fileDesc = _open_osfhandle((intptr_t)fileHandle, _O_RDONLY);
	FILE* file = _fdopen(fileDesc, "rb");
	closeFilesAtTheEnd.file = file;
	int tag;
	fread(&tag, 4, 1, file);
	if (tag != PACKAGE_FILE_TAG) {
		printf("Package file tag doesn't match.\n");
		return -1;
	}
	int fileVersion;
	fread(&fileVersion, 4, 1, file);
	printf("Main engine version: %hd\n", fileVersion & 0xffff);
	printf("Licensee version: %hd\n", (fileVersion >> 16) & 0xffff);
	int totalHeaderSize;
	fread(&totalHeaderSize, 4, 1, file);
	printf("Total header size: 0x%x\n", totalHeaderSize);
	void* copyBuf = malloc(totalHeaderSize);
	if (!copyBuf) {
		printf("Not enough memory: %d\n", totalHeaderSize);
		return -1;
	}
	int oldPos = ftell(file);
	fseek(file, 0, SEEK_SET);
	fread(copyBuf, 1, totalHeaderSize, file);
	WriteFile(writeHandle, copyBuf, totalHeaderSize, &bytesWritten, NULL);
	free(copyBuf);
	fseek(file, oldPos, SEEK_SET);
	std::wstring folderName;
	readString(folderName, file);
	printf("Foler name: %ls\n", folderName.c_str());
	DWORD packageFlags;
	fread(&packageFlags, 4, 1, file);
	printf("Package flags: (0x%x) ", packageFlags);
	printFlags(packageFlags, allPackageFlags);
	printf("\n");
	int nameCount;
	fread(&nameCount, 4, 1, file);
	printf("Name count: %d\n", nameCount);
	int nameOffset;
	fread(&nameOffset, 4, 1, file);
	printf("Name offset: 0x%x\n", nameOffset);
	int exportCount;
	fread(&exportCount, 4, 1, file);
	printf("Export count: %d\n", exportCount);
	int exportOffset;
	fread(&exportOffset, 4, 1, file);
	printf("Export offset: 0x%x\n", exportOffset);
	int importCount;
	fread(&importCount, 4, 1, file);
	printf("Import count: %d\n", importCount);
	int importOffset;
	fread(&importOffset, 4, 1, file);
	printf("Import offset: 0x%x\n", importOffset);
	int dependsOffset;
	fread(&dependsOffset, 4, 1, file);
	printf("Depends offset: 0x%x\n", dependsOffset);
	int importExportGuidOffsets = -1;
	int importGuidsCount = 0;
	int exportGuidsCount = 0;
	if ((fileVersion & 0xffff) >= 623) {
		fread(&importExportGuidOffsets, 4, 1, file);
		printf("Import export guid offsets: 0x%x\n", importExportGuidOffsets);
		fread(&importGuidsCount, 4, 1, file);
		printf("Import guids count: %d\n", importGuidsCount);
		fread(&exportGuidsCount, 4, 1, file);
		printf("Export guids count: %d\n", exportGuidsCount);
	}
	int thumbnailTableOffset = 0;
	if ((fileVersion & 0xffff) >= 584) {
		fread(&thumbnailTableOffset, 4, 1, file);
		printf("Thumbnail table offset: 0x%x\n", thumbnailTableOffset);
	}
	UEGuid guid;
	readGuid(guid, file);
	printf("Guid: ");
	printGuid(guid);
	printf("\n");
	int generationCount;
	fread(&generationCount, 4, 1, file);
	printf("Generation count: %d\n", generationCount);
	if (generationCount) {
		printf("Generations: [\n");
		for (int generationCounter = generationCount; generationCounter > 0; --generationCounter) {
			int generationExportCount;
			int generationNameCount;
			int generationNetObjectCount;
			fread(&generationExportCount, 4, 1, file);
			fread(&generationNameCount, 4, 1, file);
			fread(&generationNetObjectCount, 4, 1, file);
			printf("  {\n    Export count: %d\n", generationExportCount);
			printf("    Name count: %d\n", generationNameCount);
			printf("    Net object count: %d\n", generationNetObjectCount);
			if (generationCounter == 1) {
				printf("  }\n");
			} else {
				printf("  },\n");
			}
		}
		printf("]\n");
	}
	int engineVersion;
	fread(&engineVersion, 4, 1, file);
	printf("Engine version: %d\n", engineVersion);
	int cookedContentVersion;
	fread(&cookedContentVersion, 4, 1, file);
	printf("Cooked content version: %d\n", cookedContentVersion);
	DWORD compressionFlags;
	fread(&compressionFlags, 4, 1, file);
	printf("Compression flags: (0x%x) ", compressionFlags);
	printFlags(compressionFlags, allCompressionFlags);
	printf("\n");
	int compressedChunksCount;
	fread(&compressedChunksCount, 4, 1, file);
	if (compressedChunksCount > 0) {
		printf("Compressed chunks: [\n");
		for (int compressedChunksCounter = compressedChunksCount; compressedChunksCounter > 0; --compressedChunksCounter) {
			int uncompressedOffset;
			int uncompressedSize;
			int compressedOffset;
			int compressedSize;
			fread(&uncompressedOffset, 4, 1, file);
			fread(&uncompressedSize, 4, 1, file);
			fread(&compressedOffset, 4, 1, file);
			fread(&compressedSize, 4, 1, file);
			printf("  {\n    Uncompressed offset: 0x%x\n", uncompressedOffset);
			printf("    Uncompressed size: 0x%x\n", uncompressedSize);
			printf("    Compressed offset: 0x%x,\n", compressedOffset);
			printf("    Compressed size: 0x%x\n", compressedSize);
			if (compressedChunksCounter == 1) {
				printf("  }\n");
			} else {
				printf("  },\n");
			}
		}
		printf("]\n");
	}
	DWORD packageSource;
	fread(&packageSource, 4, 1, file);
	printf("Package source: 0x%x\n", packageSource);
	if ((fileVersion & 0xffff) >= 516) {
		int additionalPackagesToCookCount;
		fread(&additionalPackagesToCookCount, 4, 1, file);
		if (additionalPackagesToCookCount > 0) {
			printf("Additional packages to cook: [\n");
			for (int additionalPackagesToCookCounter = additionalPackagesToCookCount; additionalPackagesToCookCounter > 0; --additionalPackagesToCookCounter) {
				std::wstring packageName;
				readString(packageName, file);
				printf("  %ls", packageName.c_str());
				if (additionalPackagesToCookCounter == 1) {
					printf("\n");
				} else {
					printf(",\n");
				}
			}
			printf("]\n");
		}
	}
	if ((fileVersion & 0xffff) >= 767) {
		int textureTypesCount;
		fread(&textureTypesCount, 4, 1, file);
		if (textureTypesCount > 0) {
			printf("Texture allocations.Texture types: [\n");
			for (int textureTypesCounter = textureTypesCount; textureTypesCounter > 0; --textureTypesCounter) {
				int sizeX;
				int sizeY;
				int numMips;
				DWORD format;
				DWORD texCreateFlags;
				int exportIndicesCount;
				fread(&sizeX, 4, 1, file);
				fread(&sizeY, 4, 1, file);
				fread(&numMips, 4, 1, file);
				fread(&format, 4, 1, file);
				fread(&texCreateFlags, 4, 1, file);
				fread(&exportIndicesCount, 4, 1, file);
				printf("  {\n    Size X: %d,\n", sizeX);
				printf("    Size Y: %d,\n", sizeY);
				printf("    Num mips: %d,\n", numMips);
				printf("    Format: %d,\n", format);
				printf("    Tex create flags: 0x%x", texCreateFlags);
				if (exportIndicesCount > 0) {
					printf(",\n    Export indices: [\n");
					for (int exportIndicesCounter = exportIndicesCount; exportIndicesCounter > 0; --exportIndicesCounter) {
						int exportIndex;
						fread(&exportIndex, 4, 1, file);
						printf("      %d", exportIndex);
						if (exportIndicesCounter == 1) {
							printf("\n");
						} else {
							printf(",\n");
						}
					}
					printf("    ]");
				}
				printf("\n  }");
				if (textureTypesCounter == 1) {
					printf("\n");
				} else {
					printf(",\n");
				}
			}
			printf("]\n");
		}
	}
	if (compressionFlags != 0) {
		printf("The package is compressed. You can decompress it using gildor's decompress tool,"
		" available on his website: https://www.gildor.org/downloads\n");
		return 0;
	}
	printf("Currently at 0x%x in the file\n", ftell(file));
	std::vector<std::wstring> names;
	printf("Names: [");
	if (nameCount) {
		fseek(file, nameOffset, SEEK_SET);
		for (int nameCounter = nameCount; nameCounter > 0; --nameCounter) {
			std::wstring name;
			readString(name, file);
			names.push_back(name);
			unsigned long long contextFlags;
			fread(&contextFlags, 8, 1, file);
			printf("\n  {\n    Name: %ls,\n", name.c_str());
			printf("    Context flags: %llx\n  }", contextFlags);
			if (nameCounter != 1) {
				printf(",");
			}
		}
		printf("\n]\n");
	} else {
		printf("]\n");
	}
	std::vector<std::wstring> importNames;
	printf("Imports: [");
	if (importCount) {
		fseek(file, importOffset, SEEK_SET);
		for (int importCounter = importCount; importCounter > 0; --importCounter) {
			NameData classPackage = readNameData(names, file);
			NameData className = readNameData(names, file);
			fseek(file, 4, SEEK_CUR);
			NameData objectName = readNameData(names, file);
			importNames.push_back(nameDataToString(objectName));
		}
		fseek(file, importOffset, SEEK_SET);
		for (int importCounter = importCount; importCounter > 0; --importCounter) {
			NameData classPackage = readNameData(names, file);
			printf("\n  {\n    Class package: %ls,\n", nameDataToString(classPackage).c_str());
			NameData className = readNameData(names, file);
			printf("    Class name: %ls,\n", nameDataToString(className).c_str());
			int outerIndex;
			fread(&outerIndex, 4, 1, file);
			printf("    Outer index: %d,", outerIndex);
			if (outerIndex > 0) {
				printf("Error: outer index in imports points to exports.\n");
				return -1;
			}
			if (outerIndex) {
				printf("  // points to here, into Imports, so \"%ls\"", importNames[-outerIndex - 1].c_str());
			}
			printf("\n");
			NameData objectName = readNameData(names, file);
			printf("    Object name: %ls\n  }", nameDataToString(objectName).c_str());
			if (importCounter != 1) {
				printf(",");
			}
		}
		printf("\n]\n");
	} else {
		printf("]\n");
	}
	printf("Exports: [");
	struct Export {
		int filePositionForSizeAndOffset = 0;
		std::wstring name;
		std::vector<std::wstring> packagePath;
		std::wstring className;
		int outerIndex = 0;
	};
	std::vector<Export> exports;
	if (exportCount) {
		fseek(file, exportOffset, SEEK_SET);
		for (int exportCounter = exportCount; exportCounter > 0; --exportCounter) {
			fseek(file, 12, SEEK_CUR);
			NameData objectName = readNameData(names, file);
			Export newExport;
			newExport.name = nameDataToString(objectName);
			exports.push_back(newExport);
			fseek(file, 24, SEEK_CUR);
			int generationNetObjectCountCount;
			fread(&generationNetObjectCountCount, 4, 1, file);
			if (generationNetObjectCountCount > 0) {
				fseek(file, generationNetObjectCountCount * 4, SEEK_CUR);
			}
			fseek(file, 20, SEEK_CUR);
		}
		fseek(file, exportOffset, SEEK_SET);
		for (int exportCounter = exportCount; exportCounter > 0; --exportCounter) {
			Export& exportStruct = exports[exportCount - exportCounter];
			int classIndex;
			fread(&classIndex, 4, 1, file);
			printf("\n  {\n    Class index: %d,", classIndex);
			if (classIndex) {
				if (classIndex < 0) {
					printf("  // imports[%d]: %ls", -classIndex - 1, importNames[-classIndex - 1].c_str());
					exportStruct.className = importNames[-classIndex - 1];
				} else {
					printf("  // exports[%d]: %ls", classIndex - 1, exports[classIndex - 1].name.c_str());
					exportStruct.className = exports[classIndex - 1].name;
				}
			}
			printf("\n");
			int superIndex;
			fread(&superIndex, 4, 1, file);
			printf("    Super index: %d,", superIndex);
			if (superIndex) {
				if (superIndex < 0) {
					printf("  // imports[%d]: %ls", -superIndex - 1, importNames[-superIndex - 1].c_str());
				} else {
					printf("  // exports[%d]: %ls", superIndex - 1, exports[superIndex - 1].name.c_str());
				}
			}
			printf("\n");
			int outerIndex;
			fread(&outerIndex, 4, 1, file);
			printf("    Outer index: %d,", outerIndex);
			if (outerIndex) {
				if (outerIndex < 0) {
					printf("  // imports[%d]: %ls", -outerIndex - 1, importNames[-outerIndex - 1].c_str());
				} else {
					printf("  // exports[%d]: %ls", outerIndex - 1, exports[outerIndex - 1].name.c_str());
				}
			}
			printf("\n");
			exportStruct.outerIndex = outerIndex;
			NameData objectName = readNameData(names, file);
			printf("    Object name: %ls,\n", nameDataToString(objectName).c_str());
			int archetypeIndex;
			fread(&archetypeIndex, 4, 1, file);
			printf("    Archetype index: %d,", archetypeIndex);
			if (archetypeIndex) {
				if (archetypeIndex < 0) {
					printf("  // imports[%d]: %ls", -archetypeIndex - 1, importNames[-archetypeIndex - 1].c_str());
				} else {
					printf("  // exports[%d]: %ls", archetypeIndex - 1, exports[archetypeIndex - 1].name.c_str());
				}
			}
			printf("\n");
			unsigned long long objectFlags;
			fread(&objectFlags, 8, 1, file);
			printf("    Object flags: 0x%llx,\n", objectFlags);
			exportStruct.filePositionForSizeAndOffset = ftell(file);
			int serializeSize;
			fread(&serializeSize, 4, 1, file);
			printf("    Serialize size: 0x%x,\n", serializeSize);
			int serialOffset;
			fread(&serialOffset, 4, 1, file);
			printf("    Serial offset: 0x%x,\n", serialOffset);
			if ((fileVersion & 0xffff) < 543) {
				int len;
				fread(&len, 4, 1, file);
				fseek(file, len * 3 * 4, SEEK_CUR);
			}
			DWORD exportFlags;
			fread(&exportFlags, 4, 1, file);
			printf("    Export flags: (0x%x) ", exportFlags);
			printFlags(exportFlags, allExportFlags, "    ");
			printf(",\n");
			int generationNetObjectCountCount;
			fread(&generationNetObjectCountCount, 4, 1, file);
			if (generationNetObjectCountCount > 0) {
				printf("    Generation net object count: [\n");
				for (int generationNetObjectCountCounter = generationNetObjectCountCount; generationNetObjectCountCounter > 0; --generationNetObjectCountCounter) {
					int count;
					fread(&count, 4, 1, file);
					printf("      %d", count);
					if (generationNetObjectCountCounter != 1) {
						printf(",");
					}
					printf("\n");
				}
				printf("    ],\n");
			}
			UEGuid exportGuid;
			readGuid(exportGuid, file);
			printf("    Guid: ");
			printGuid(exportGuid);
			printf(",\n");
			DWORD exportPackageFlags;
			fread(&exportPackageFlags, 4, 1, file);
			printf("    Package flags: 0x%x\n  }", exportPackageFlags);
			if (exportCounter != 1) {
				printf(",");
			}
		}
		for (int exportCounter = exportCount; exportCounter > 0; --exportCounter) {
			Export& exportStruct = exports[exportCount - exportCounter];
			int outerIndexIter = exportStruct.outerIndex;
			while (outerIndexIter) {
				if (outerIndexIter < 0) {
					break;
				} else {
					exportStruct.packagePath.push_back(exports[outerIndexIter - 1].name);
					outerIndexIter = exports[outerIndexIter - 1].outerIndex;
				}
			}
			std::reverse(exportStruct.packagePath.begin(), exportStruct.packagePath.end());
		}
		printf("\n]\n");
	} else {
		printf("]\n");
	}
	if (!exportCount) return 0;
	fseek(file, exports[0].filePositionForSizeAndOffset + 4, SEEK_SET);
	int currentOffset;
	fread(&currentOffset, 4, 1, file);
	for (int exportCounter = exportCount; exportCounter > 0; --exportCounter) {
		Export& exportStruct = exports[exportCount - exportCounter];
		std::wstring fullPath = argv[2];
		if (!fullPath.empty() && fullPath[fullPath.size() - 1] != L'\\') {
			fullPath += L'\\';
		}
		for (std::wstring& pathElem : exportStruct.packagePath) {
			fullPath += pathElem + L'\\';
		}
		fullPath += exportStruct.name + L'.' + exportStruct.className;
		if (!fileExists(fullPath.c_str())) {
			printf("File not found: %ls\n", fullPath.c_str());
			return -1;
		}
		HANDLE resourceFileHandle = CreateFileW(
			fullPath.c_str(),
			GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD resourceFileSize = GetFileSize(resourceFileHandle, NULL);
		void* resourceBuf = malloc(resourceFileSize);
		if (!resourceBuf) {
			CloseHandle(resourceFileHandle);
			printf("Failed to allocate memory: %d\n", resourceFileSize);
			return -1;
		}
		if (!ReadFile(resourceFileHandle, resourceBuf, resourceFileSize, &bytesWritten, NULL)) {
			WinError err;
			printf("Failed to read file %ls: %ls\n", fullPath.c_str(), err.getMessage());
			return -1;
		}
		CloseHandle(resourceFileHandle);
		SetFilePointer(writeHandle, currentOffset, NULL, FILE_BEGIN);
		WriteFile(writeHandle, resourceBuf, resourceFileSize, &bytesWritten, NULL);
		free(resourceBuf);
		SetFilePointer(writeHandle, exportStruct.filePositionForSizeAndOffset, NULL, FILE_BEGIN);
		WriteFile(writeHandle, &resourceFileSize, 4, &bytesWritten, NULL);
		WriteFile(writeHandle, &currentOffset, 4, &bytesWritten, NULL);
		currentOffset += resourceFileSize;
	}

	return 0;
}
