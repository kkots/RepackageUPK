

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
			printf("  \"%s\"", fwn.name);
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
	"Simple UE3 .UPK repackager or info printer.\n"
	"\n"
	"Usage 1:\n"
	" Repackage UPK. Requires the original .UPK file and the result of its extraction via"
	" gildor's extract tool (can be obtained at his website: https://www.gildor.org/downloads)."
	" Will copy the .UPK and replace all the files in it with the ones found in the extracted folder."
	" Cannot add, remove any of the files or change their classes or paths within the package etc.\n"
	"\n"
	" Syntax:\n"
	"   RepackageUPK [-dataOnly] [-info] ORIGINAL_UPK EXTRACTED_FOLDER NEW_UPK\n"
	" , where:\n"
	"   ORIGINAL_UPK is the path to the original .UPK file that you want to make a copy of,\n"
	"   EXTRACTED_FOLDER is the path to the folder into which you extracted the contents of the\n"
	"       ORIGINAL_UPK with gildor's tool, and which contains the modified files as well,\n"
	"   NEW_UPK is the path, including the name and the extension, where a new .UPK copy will\n"
	"       be created with the modified files.\n"
	"       The original .UPK will not be modified.\n"
	"   -dataOnly is an optional flag that prevents the tool from printing comments intended\n"
	"       to be read by the user that are not part of JSON data structure.\n"
	"       Such comments will however still be printed on error.\n"
	"   -info is an optional flag that makes the tool also print the same info it would\n"
	"       print in the Usage 2 mode while performing the repackage operation.\n"
	"\n"
	"Usage 2:\n"
	" List contents of and information about the UPK.\n"
	" Syntax:\n"
	"   RepackageUPK -info [-dataOnly] ORIGINAL_UPK"
	);
}

// Escapes non-ASCII characters, \, " and control characters just the way python json.dumps(ensure_ascii=True) does it.
// Only thing this doesn't do is it put quotation marks around the resulting string.
// And it prints it to stdout, not to a buffer.
void printWStrAsJsonEscapedUnicode(const wchar_t* txt) {
	while (*txt != L'\0') {
		unsigned int codePoint = *txt;
		++txt;
		if (codePoint >= 0x20 && codePoint <= 126) {
			printf("%c", codePoint);
		} else if (codePoint == '\n') {
			printf("%s", "\\n");
		} else if (codePoint == '\t') {
			printf("%s", "\\t");
		} else if (codePoint == '\r') {
			printf("%s", "\\r");
		} else if (codePoint == '\f') {
			printf("%s", "\\f");
		} else if (codePoint == '\b') {
			printf("%s", "\\b");
		} else if (codePoint == '\\') {
			printf("%s", "\\\\");
		} else if (codePoint == '\"') {
			printf("%s", "\\\"");
		} else {
			printf("\\u%.4x", codePoint);
		}
	}
}

// To put UTF-8 string literals into C prepend them with u8 prefix and save the file as UTF-8.
void printUtf8StrAsJsonEscapedUnicode(const char* txt) {
	int requiredSize = MultiByteToWideChar(CP_UTF8, NULL, txt, -1, NULL, 0);
	if (!requiredSize) return;
	wchar_t* buf = (wchar_t*)malloc(requiredSize * sizeof(wchar_t));
	if (!buf) return;
	MultiByteToWideChar(CP_UTF8, NULL, txt, -1, buf, requiredSize);
	printWStrAsJsonEscapedUnicode(buf);
	free(buf);
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

	wchar_t* otherThreeArgs[3] { nullptr };
	int otherThreeArgsCounter = 0;
	bool isInfo = false;
	bool isDataOnly = false;
	for (int i = 1; i < argc; ++i) {
		wchar_t* option = argv[i];
		if (_wcsicmp(option, L"-info") == 0) {
			isInfo = true;
		} else if (_wcsicmp(option, L"-dataOnly") == 0) {
			isDataOnly = true;
		} else {
			if (otherThreeArgsCounter >= _countof(otherThreeArgs)) {
				printHelp();
				return -1;
			}
			otherThreeArgs[otherThreeArgsCounter] = option;
			++otherThreeArgsCounter;
		}
	}

	bool isRepackageMode = (otherThreeArgsCounter == 3);
	if (!isRepackageMode && !isInfo
			|| !isRepackageMode && isInfo && otherThreeArgsCounter != 1) {
		printHelp();
		return (argc == 1 ? 0 : -1);
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
		otherThreeArgs[0],
		GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fileHandle == INVALID_HANDLE_VALUE) {
		std::cout << "Failed to open file\n";
		return -1;
	}
	DWORD bytesWritten = 0;
	HANDLE writeHandle = NULL;
	if (isRepackageMode) {
		closeFilesAtTheEnd.writeHandle = fileHandle;
		const wchar_t* writeFileName = otherThreeArgs[2];
		writeHandle = CreateFileW(writeFileName,
			GENERIC_WRITE, NULL, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (writeHandle == INVALID_HANDLE_VALUE) {
			WinError err;
			std::cout << "Failed to create file at location: ";
			std::wcout << writeFileName << L'\n' << err.getMessage() << L'\n';
			return -1;
		}
		closeFilesAtTheEnd.writeHandle = writeHandle;
	}
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
	if (isInfo) {
		printf("{\n  \"Main engine version\": %hd,\n", fileVersion & 0xffff);
		printf("  \"Licensee version\": %hd,\n", (fileVersion >> 16) & 0xffff);
	}
	int totalHeaderSize;
	fread(&totalHeaderSize, 4, 1, file);
	if (isInfo) {
		printf("  \"Total header size\": \"0x%x\",\n", totalHeaderSize);
	}
	void* copyBuf = malloc(totalHeaderSize);
	if (!copyBuf) {
		printf("Not enough memory: %d\n", totalHeaderSize);
		return -1;
	}
	int oldPos = ftell(file);
	fseek(file, 0, SEEK_SET);
	fread(copyBuf, 1, totalHeaderSize, file);
	if (isRepackageMode) {
		WriteFile(writeHandle, copyBuf, totalHeaderSize, &bytesWritten, NULL);
	}
	free(copyBuf);
	fseek(file, oldPos, SEEK_SET);
	std::wstring folderName;
	readString(folderName, file);
	if (isInfo) {
		printf("  \"Foler name\": \"");
		printWStrAsJsonEscapedUnicode(folderName.c_str());
		printf("\",\n");
	}
	DWORD packageFlags;
	fread(&packageFlags, 4, 1, file);
	if (isInfo) {
		printf("  \"Package flags\": \"0x%x\",\n", packageFlags);
		printf("  \"Package flags list\": ");
		printFlags(packageFlags, allPackageFlags, "  ");
		printf(",\n");
	}
	int nameCount;
	fread(&nameCount, 4, 1, file);
	if (isInfo) {
		printf("  \"Name count\": %d,\n", nameCount);
	}
	int nameOffset;
	fread(&nameOffset, 4, 1, file);
	if (isInfo) {
		printf("  \"Name offset\": \"0x%x\",\n", nameOffset);
	}
	int exportCount;
	fread(&exportCount, 4, 1, file);
	if (isInfo) {
		printf("  \"Export count\": %d,\n", exportCount);
	}
	int exportOffset;
	fread(&exportOffset, 4, 1, file);
	if (isInfo) {
		printf("  \"Export offset\": \"0x%x\",\n", exportOffset);
	}
	int importCount;
	fread(&importCount, 4, 1, file);
	if (isInfo) {
		printf("  \"Import count\": %d,\n", importCount);
	}
	int importOffset;
	fread(&importOffset, 4, 1, file);
	if (isInfo) {
		printf("  \"Import offset\": \"0x%x\",\n", importOffset);
	}
	int dependsOffset;
	fread(&dependsOffset, 4, 1, file);
	if (isInfo) {
		printf("  \"Depends offset\": \"0x%x\",\n", dependsOffset);
	}
	int importExportGuidOffsets = -1;
	int importGuidsCount = 0;
	int exportGuidsCount = 0;
	if ((fileVersion & 0xffff) >= 623) {
		fread(&importExportGuidOffsets, 4, 1, file);
		if (isInfo) {
			printf("  \"Import export guid offsets\": \"0x%x\",\n", importExportGuidOffsets);
		}
		fread(&importGuidsCount, 4, 1, file);
		if (isInfo) {
			printf("  \"Import guids count\": %d,\n", importGuidsCount);
		}
		fread(&exportGuidsCount, 4, 1, file);
		if (isInfo) {
			printf("  \"Export guids count\": %d,\n", exportGuidsCount);
		}
	}
	int thumbnailTableOffset = 0;
	if ((fileVersion & 0xffff) >= 584) {
		fread(&thumbnailTableOffset, 4, 1, file);
		if (isInfo) {
			printf("  \"Thumbnail table offset\": \"0x%x\",\n", thumbnailTableOffset);
		}
	}
	UEGuid guid;
	readGuid(guid, file);
	if (isInfo) {
		printf("  \"Guid\": \"");
		printGuid(guid);
		printf("\",\n");
	}
	int generationCount;
	fread(&generationCount, 4, 1, file);
	if (isInfo) {
		printf("  \"Generation count\": %d,\n", generationCount);
	}
	if (generationCount) {
		if (isInfo) {
			printf("  \"Generations\": [\n");
		}
		for (int generationCounter = generationCount; generationCounter > 0; --generationCounter) {
			int generationExportCount;
			int generationNameCount;
			int generationNetObjectCount;
			fread(&generationExportCount, 4, 1, file);
			fread(&generationNameCount, 4, 1, file);
			fread(&generationNetObjectCount, 4, 1, file);
			if (isInfo) {
				printf("    {\n      \"Export count\": %d,\n", generationExportCount);
				printf("      \"Name count\": %d,\n", generationNameCount);
				printf("      \"Net object count\": %d\n", generationNetObjectCount);
				if (generationCounter == 1) {
					printf("    }\n");
				} else {
					printf("    },\n");
				}
			}
		}
		if (isInfo) {
			printf("  ],\n");
		}
	}
	int engineVersion;
	fread(&engineVersion, 4, 1, file);
	if (isInfo) {
		printf("  \"Engine version\": %d,\n", engineVersion);
	}
	int cookedContentVersion;
	fread(&cookedContentVersion, 4, 1, file);
	if (isInfo) {
		printf("  \"Cooked content version\": %d,\n", cookedContentVersion);
	}
	DWORD compressionFlags;
	fread(&compressionFlags, 4, 1, file);
	if (isInfo) {
		printf("  \"Compression flags\": \"0x%x\",\n", compressionFlags);
		printf("  \"Compression flags list\": ");
		printFlags(compressionFlags, allCompressionFlags, "  ");
		printf(",\n");
	}
	int compressedChunksCount;
	fread(&compressedChunksCount, 4, 1, file);
	if (compressedChunksCount > 0) {
		if (isInfo) {
			printf("  \"Compressed chunks\": [\n");
		}
		for (int compressedChunksCounter = compressedChunksCount; compressedChunksCounter > 0; --compressedChunksCounter) {
			int uncompressedOffset;
			int uncompressedSize;
			int compressedOffset;
			int compressedSize;
			fread(&uncompressedOffset, 4, 1, file);
			fread(&uncompressedSize, 4, 1, file);
			fread(&compressedOffset, 4, 1, file);
			fread(&compressedSize, 4, 1, file);
			if (isInfo) {
				printf("    {\n      \"Uncompressed offset\": \"0x%x\",\n", uncompressedOffset);
				printf("      \"Uncompressed size\": \"0x%x\",\n", uncompressedSize);
				printf("      \"Compressed offset\": \"0x%x\",\n", compressedOffset);
				printf("      \"Compressed size\": \"0x%x\"\n", compressedSize);
				if (compressedChunksCounter == 1) {
					printf("    }\n");
				} else {
					printf("    },\n");
				}
			}
		}
		if (isInfo) {
			printf("  ],\n");
		}
	}
	DWORD packageSource;
	fread(&packageSource, 4, 1, file);
	if (isInfo) {
		printf("  \"Package source\": \"0x%x\",\n", packageSource);
	}
	if ((fileVersion & 0xffff) >= 516) {
		int additionalPackagesToCookCount;
		fread(&additionalPackagesToCookCount, 4, 1, file);
		if (additionalPackagesToCookCount > 0) {
			if (isInfo) {
				printf("  \"Additional packages to cook\": [\n");
			}
			for (int additionalPackagesToCookCounter = additionalPackagesToCookCount; additionalPackagesToCookCounter > 0; --additionalPackagesToCookCounter) {
				std::wstring packageName;
				readString(packageName, file);
				if (isInfo) {
					printf("    \"");
					printWStrAsJsonEscapedUnicode(packageName.c_str());
					printf("\"");
					if (additionalPackagesToCookCounter == 1) {
						printf("\n");
					} else {
						printf(",\n");
					}
				}
			}
			if (isInfo) {
				printf("  ],\n");
			}
		}
	}
	if ((fileVersion & 0xffff) >= 767) {
		int textureTypesCount;
		fread(&textureTypesCount, 4, 1, file);
		if (textureTypesCount > 0) {
			if (isInfo) {
				printf("  \"Texture allocations.Texture types\": [\n");
			}
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
				if (isInfo) {
					printf("    {\n      \"Size X\": %d,\n", sizeX);
					printf("      \"Size Y\": %d,\n", sizeY);
					printf("      \"Num mips\": %d,\n", numMips);
					printf("      \"Format\": %d,\n", format);
					printf("      \"Tex create flags\": \"0x%x\"", texCreateFlags);
				}
				if (exportIndicesCount > 0) {
					if (isInfo) {
						printf(",\n      \"Export indices\": [\n");
					}
					for (int exportIndicesCounter = exportIndicesCount; exportIndicesCounter > 0; --exportIndicesCounter) {
						int exportIndex;
						fread(&exportIndex, 4, 1, file);
						if (isInfo) {
							printf("        %d", exportIndex);
							if (exportIndicesCounter == 1) {
								printf("\n");
							} else {
								printf(",\n");
							}
						}
					}
					if (isInfo) {
						printf("      ]");
					}
				}
				if (isInfo) {
					printf("\n    }");
					if (textureTypesCounter == 1) {
						printf("\n");
					} else {
						printf(",\n");
					}
				}
			}
			if (isInfo) {
				printf("  ],\n");
			}
		}
	}
	if (compressionFlags != 0) {
		if (!isDataOnly) {
			printf("The package is compressed. You can decompress it using gildor's decompress tool,"
			" available on his website: https://www.gildor.org/downloads\n");
		}
		return 0;
	}
	std::vector<std::wstring> names;
	if (isInfo) {
		printf("  \"Names\": [");
	}
	if (nameCount) {
		fseek(file, nameOffset, SEEK_SET);
		for (int nameCounter = nameCount; nameCounter > 0; --nameCounter) {
			std::wstring name;
			readString(name, file);
			names.push_back(name);
			unsigned long long contextFlags;
			fread(&contextFlags, 8, 1, file);
			if (isInfo) {
				printf("\n    {\n      \"Name\": \"");
				printWStrAsJsonEscapedUnicode(name.c_str());
				printf("\",\n");
				printf("      \"Context flags\": \"%llx\"\n    }", contextFlags);
				if (nameCounter != 1) {
					printf(",");
				}
			}
		}
		if (isInfo) {
			printf("\n  ],\n");
		}
	} else if (isInfo) {
		printf("  ],\n");
	}
	struct Import {
		std::wstring name;
		NameData classPackage;
		NameData className;
		int outerIndex;
		NameData objectName;
	};
	std::vector<Import> imports;
	if (importCount) {
		fseek(file, importOffset, SEEK_SET);
		for (int importCounter = importCount; importCounter > 0; --importCounter) {
			imports.emplace_back();
			Import& importStruct = imports.back();
			importStruct.classPackage = readNameData(names, file);
			importStruct.className = readNameData(names, file);
			fread(&importStruct.outerIndex, 4, 1, file);
			importStruct.objectName = readNameData(names, file);
			importStruct.name = nameDataToString(importStruct.objectName);
		}
	}
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
	}
	if (!importCount && isInfo) printf("  \"Imports\": [],\n");
	if (importCount && isInfo) {
		printf("  \"Imports\": [");
		int importCounter = 0;
		for (Import& importStruct : imports) {
			printf("\n    {\n      \"Class package\": \"");
			printWStrAsJsonEscapedUnicode(nameDataToString(importStruct.classPackage).c_str());
			printf("\",\n");
			printf("      \"Class name\": \"");
			printWStrAsJsonEscapedUnicode(nameDataToString(importStruct.className).c_str());
			printf("\",\n");
			printf("      \"Outer index\": %d,\n", importStruct.outerIndex);
			if (importStruct.outerIndex > 0) {
				printf("      \"Outer index comment\": \"// points to exports, so \\\"");
				printWStrAsJsonEscapedUnicode(exports[importStruct.outerIndex - 1].name.c_str());
				printf("\\\"\",\n");
			}
			if (importStruct.outerIndex < 0) {
				printf("      \"Outer index comment\": \"// points to here, into Imports, so \\\"");
				printWStrAsJsonEscapedUnicode(imports[-importStruct.outerIndex - 1].name.c_str());
				printf("\\\"\",\n");
			}
			printf("      \"Object name\": \"");
			printWStrAsJsonEscapedUnicode(nameDataToString(importStruct.objectName).c_str());
			printf("\"\n    }");
			if (importCounter != imports.size() - 1) {
				printf(",");
			}
			++importCounter;
		}
		printf("\n  ],\n");
	}
	if (isInfo) {
		printf("  \"Exports\": [");
	}
	if (exportCount) {
		fseek(file, exportOffset, SEEK_SET);
		int exportCounterStraight = 0;
		for (int exportCounter = exportCount; exportCounter > 0; --exportCounter) {
			Export& exportStruct = exports[exportCount - exportCounter];
			int classIndex;
			fread(&classIndex, 4, 1, file);
			if (isInfo) {
				printf("\n    {\n      \"Class index\": %d,\n", classIndex);
			}
			if (classIndex) {
				if (classIndex < 0) {
					if (isInfo) {
						printf("      \"Class index comment\": \"// imports[%d]: \\\"", -classIndex - 1);
						printWStrAsJsonEscapedUnicode(imports[-classIndex - 1].name.c_str());
						printf("\\\"\",\n");
					}
					exportStruct.className = imports[-classIndex - 1].name;
				} else {
					if (isInfo) {
						printf("      \"Class index comment\": \"// exports[%d]: \\\"", classIndex - 1);
						printWStrAsJsonEscapedUnicode(exports[classIndex - 1].name.c_str());
						printf("\\\"\",\n");
					}
					exportStruct.className = exports[classIndex - 1].name;
				}
			}
			int superIndex;
			fread(&superIndex, 4, 1, file);
			if (isInfo) {
				printf("      \"Super index\": %d,\n", superIndex);
			}
			if (superIndex && isInfo) {
				if (superIndex < 0) {
					printf("      \"Super index comment\": \"// imports[%d]: \\\"", -superIndex - 1);
					printWStrAsJsonEscapedUnicode(imports[-superIndex - 1].name.c_str());
					printf("\\\"\",\n");
				} else {
					printf("      \"Super index comment\": \"// exports[%d]: \\\"", superIndex - 1);
					printWStrAsJsonEscapedUnicode(exports[superIndex - 1].name.c_str());
					printf("\\\"\",\n");
				}
			}
			int outerIndex;
			fread(&outerIndex, 4, 1, file);
			if (isInfo) {
				printf("      \"Outer index\": %d,\n", outerIndex);
			}
			if (outerIndex && isInfo) {
				if (outerIndex < 0) {
					printf("      \"Outer index comment\": \"// imports[%d]: \\\"", -outerIndex - 1);
					printWStrAsJsonEscapedUnicode(imports[-outerIndex - 1].name.c_str());
					printf("\\\"\",\n");
				} else {
					printf("      \"Outer index comment\": \"// exports[%d]: \\\"", outerIndex - 1);
					printWStrAsJsonEscapedUnicode(exports[outerIndex - 1].name.c_str());
					printf("\\\"\",\n");
				}
			}
			exportStruct.outerIndex = outerIndex;
			NameData objectName = readNameData(names, file);
			if (isInfo) {
				printf("      \"Object name\": \"");
				printWStrAsJsonEscapedUnicode(nameDataToString(objectName).c_str());
				printf("\",\n");
			}
			int archetypeIndex;
			fread(&archetypeIndex, 4, 1, file);
			if (isInfo) {
				printf("      \"Archetype index\": %d,\n", archetypeIndex);
			}
			if (archetypeIndex && isInfo) {
				if (archetypeIndex < 0) {
					printf("      \"Archetype index comment\": \"// imports[%d]: \\\"", -archetypeIndex - 1);
					printWStrAsJsonEscapedUnicode(imports[-archetypeIndex - 1].name.c_str());
					printf("\\\"\",\n");
				} else {
					printf("      \"Archetype index comment\": \"// exports[%d]: \\\"", archetypeIndex - 1);
					printWStrAsJsonEscapedUnicode(exports[archetypeIndex - 1].name.c_str());
					printf("\\\"\",\n");
				}
			}
			unsigned long long objectFlags;
			fread(&objectFlags, 8, 1, file);
			if (isInfo) {
				printf("      \"Object flags\": \"0x%llx\",\n", objectFlags);
			}
			exportStruct.filePositionForSizeAndOffset = ftell(file);
			int serializeSize;
			fread(&serializeSize, 4, 1, file);
			if (isInfo) {
				printf("      \"Serialize size\": \"0x%x\",\n", serializeSize);
			}
			int serialOffset;
			fread(&serialOffset, 4, 1, file);
			if (isInfo) {
				printf("      \"Serial offset\": \"0x%x\",\n", serialOffset);
			}
			if ((fileVersion & 0xffff) < 543) {
				int len;
				fread(&len, 4, 1, file);
				fseek(file, len * 3 * 4, SEEK_CUR);
			}
			DWORD exportFlags;
			fread(&exportFlags, 4, 1, file);
			if (isInfo) {
				printf("      \"Export flags\": \"0x%x\",\n", exportFlags);
				printf("      \"Export flags list\": ");
				printFlags(exportFlags, allExportFlags, "      ");
				printf(",\n");
			}
			int generationNetObjectCountCount;
			fread(&generationNetObjectCountCount, 4, 1, file);
			if (generationNetObjectCountCount > 0) {
				if (isInfo) {
					printf("      \"Generation net object count\": [\n");
				}
				for (int generationNetObjectCountCounter = generationNetObjectCountCount; generationNetObjectCountCounter > 0; --generationNetObjectCountCounter) {
					int count;
					fread(&count, 4, 1, file);
					if (isInfo) {
						printf("        %d", count);
						if (generationNetObjectCountCounter != 1) {
							printf(",");
						}
						printf("\n");
					}
				}
				if (isInfo) {
					printf("      ],\n");
				}
			}
			UEGuid exportGuid;
			readGuid(exportGuid, file);
			if (isInfo) {
				printf("      \"Guid\": \"");
				printGuid(exportGuid);
				printf("\",\n");
			}
			DWORD exportPackageFlags;
			fread(&exportPackageFlags, 4, 1, file);
			if (isInfo) {
				printf("      \"Index\": %d,\n", exportCounterStraight);
				printf("      \"Package flags\": \"0x%x\"\n    }", exportPackageFlags);
				if (exportCounter != 1) {
					printf(",");
				}
			}
			++exportCounterStraight;
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
		if (isInfo) {
			printf("\n  ]\n");
		}
	} else if (isInfo) {
		printf("  ]\n");
	}
	if (isInfo) {
		printf("}\n");
	}
	if (!isRepackageMode) return 0;
	if (!exportCount) return 0;
	fseek(file, exports[0].filePositionForSizeAndOffset + 4, SEEK_SET);
	int currentOffset;
	fread(&currentOffset, 4, 1, file);
	for (int exportCounter = exportCount; exportCounter > 0; --exportCounter) {
		Export& exportStruct = exports[exportCount - exportCounter];
		std::wstring fullPath = otherThreeArgs[1];
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
