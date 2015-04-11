/*
 * This file is part of the Guardian Project: https://github.com/spazedog/guardian
 *
 * Copyright (c) 2015 Daniel Bergløv
 *
 * Guardian is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Guardian is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with Guardian. If not, see <http://www.gnu.org/licenses/>
 */

#include <jni.h>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include <cctype>
#include <cstdlib>

#define DEBUG_TAG "NDK_GuardianScanner"

/*
 * This native library is used to collect the stat's of all the currently running processes on a device.
 * This task has been assigned to C++ because it is much faster at file operations compared to Java.
 * C++ actually does a well enough job in this area that it is able to do this work 87% faster than
 * Java, which was a result after some extensive testing in both languages.
 *
 * Java is fine to use for file operations. Just not when you need to extract data from
 * 250-500 files at once.
 */

extern "C" {

	/**
	 * =====================================================================
	 * ---------------------------------------------------------------------
	 */
	static std::string fixProcessNameSyntax(std::string name) {
		size_t posSl = name.find('/');
		size_t posCn = name.find('-');
		std::string ret;

		if (posSl != std::string::npos) {
			if (posSl > 0) {
				ret = name.substr(0, posSl);

			} else {
				/*
				 * Some cmdline files contains something like "/system/bin/binary--command-args"
				 * We only want the name, not the path or the args.
				 */
				size_t startPos = (posCn != std::string::npos) ? name.rfind('/', posCn)+1 : name.rfind('/')+1;
				size_t endPos = (posCn != std::string::npos) ? name.find('-', startPos) : name.size();

				ret = name.substr(startPos, endPos);
			}

		} else if (posCn != std::string::npos) {
			ret = name.substr(0, posCn);

		} else {
			return name;
		}

		return ret;
	}

	/**
	 * =====================================================================
	 * ---------------------------------------------------------------------
	 */

	static std::vector<std::string> splitStatLine(std::string& str) {
		std::vector<std::string> lines;

		/*
		 * We have two types of files (/proc/stat) and (/proc/<pid>/stat) which differs a little.
		 *
		 *  - /proc/stat = cpu 7650947 104625 1567588 52176062 744598 139 40697 0 0 0
		 *  - /proc/<pid>/stat = 21 (migration/2) S 2 0 0 0 -1 69247040 0 0 0 0 0 44 0 0 -100 0 1 0 7 0 .........
		 *
		 *  The below code can handle both these types of files.
		 *
		 *  	CPU Stat (/proc/stat)
		 *
		 *  		- [0] = Type (cpu)
		 *  		- [1] = Total idle time
		 *  		- [2] = Total uptime (Including idle)
		 *
		 *
		 *  	Process Stat (/proc/<pid>/stat)
		 *
		 *  		- [0] = Type (0 or Android importance level)
		 *  		- [1] = Process ID (pid)
		 *  		- [2] = Process Name
		 *  		- [3] = UTime
		 *  		- [4] = STime
		 *  		- [5] = CUTime
		 *  		- [6] = CSTime
		 *  		- [7] = Process uptime (The cpu total uptime at process launch)
		 */
		bool isGrouped = false;
		std::string current = "";
		long currentInt = 0;
		int i = 0;
		bool cpu = false;

		for(char& c : str) {
			if (i == 0 && c == 'c') {  // The CPU stat starts with 'cpu'
				cpu = true;
			}

			if (std::isspace(c) && !isGrouped) {
				if (!current.empty()) {
					/*
					 * Create one single Total Uptime from the CPU stat
					 */
					if (cpu && i > 0) {
						currentInt += std::atol(current.c_str());

						if (i == 5) {
							lines.push_back(current);
						}

					} else {
						lines.push_back(current);
					}

					current = "";
				}

				i += 1;  // Count for each section that has past (divided by space, except for process names)

			} else if (!cpu && (c == '(' || c == ')')) {
				/*
				 * Ignorer space while inside (...)
				 */
				isGrouped = c == '(';

			} else if (cpu || ((i >= 0 && i <= 2) || (i >= 14 && i <= 17) || i == 22)) {
				current += c;
			}
		}

		if (cpu) {
			std::stringstream stream;
			stream << currentInt;

			lines.push_back( stream.str() );
		}

		return lines;
	}

	/**
	 * =====================================================================
	 * ---------------------------------------------------------------------
	 */
	static jobjectArray createProcessData(JNIEnv* env, std::vector<std::string>& lines) {
		/*
		 * We will also need to collect the /proc/<pid>/cmdline as this file
		 * will contain the full process name whenever the one in /proc/<pid>/stat
		 * has been truncated.
		 */
		if (lines.size() > 0 && lines[0] != "cpu") {
			std::ifstream in( (std::string("/proc/") + lines[1] + "/cmdline").c_str() );

			if (in && in.good()) {
				std::string line = "";
				std::getline(in, line);

				if (!line.empty()) {
					lines[2] = fixProcessNameSyntax(line);
				}
			}

			if (in) {
				in.close();
			}
		}

		/*
		 * Now we just need to add all of the data to a Java String array
		 */
		jobjectArray ret = env->NewObjectArray(lines.size(), env->FindClass("java/lang/String"), NULL);

		for (int i = 0; i < lines.size(); i++) {
			jstring stringObject = env->NewStringUTF( lines[i].c_str() );
			env->SetObjectArrayElement(ret, i, stringObject);
			env->DeleteLocalRef(stringObject);
		}

		return ret;
	}

	/**
	 * =====================================================================
	 * ---------------------------------------------------------------------
	 */
	static bool isIntegerString(std::string str) {
		for (size_t n = 0; n < str.length(); n++) {
			if (!isdigit( str[ n ] )) {
				return false;
			}
		}

		return str.length() > 0;
	}

	/**
	 * =====================================================================
	 * ---------------------------------------------------------------------
	 */
	JNIEXPORT jobjectArray JNICALL Java_com_spazedog_guardian_scanner_ProcessScanner_getProcessList(JNIEnv *env, jobject thisObj, jintArray processList, jboolean collectFromList) {
		/*
		 * Whether or not to collect all processes or only the once defined in the processList
		 */
		bool listCollection = collectFromList == JNI_TRUE;

		/*
		 * Unpack the predefined process list from JVM
		 */
		std::vector< std::pair<std::string, std::string> > typeList;

		if (processList != NULL) {
			int size = env->GetArrayLength(processList);
			jint* body = env->GetIntArrayElements(processList, 0);

			for (int i=0, x=1; x < size; i += 2, x += 2) {
				std::stringstream pidStream;
				std::stringstream typeStream;

				pidStream << body[i];
				typeStream << body[x];

				typeList.push_back( std::pair<std::string, std::string>(pidStream.str(), typeStream.str()) );
			}
		}

		/*
		 * Start collecting data from /proc
		 */
		std::vector<std::string> lines;
		std::ifstream in("/proc/stat");
		struct dirent* procEntity;
		std::string cpuLine = "";

		if (in && in.good()) {
			std::getline(in, cpuLine);

			lines.push_back(cpuLine);
		}

		if (in) {
			in.close();
		}

		if (!listCollection || typeList.size() > 0) {
			DIR* procDirectory = opendir("/proc");

			if (procDirectory != NULL) {
				while ((procEntity = readdir(procDirectory)) != NULL) {
					std::string type = "0";
					std::string entityName = procEntity->d_name;
					bool isProcess = isIntegerString(entityName);
					bool isListedProcess = false;

					/*
					 * If isProcess is false, there is no need to search the 'Type List'
					 */
					if (isProcess) {
						for (std::vector< std::pair<std::string, std::string> >::iterator it = typeList.begin() ; it != typeList.end(); ++it) {
							if (it->first == entityName) {
								type = std::string( it->second );
								isListedProcess = true;

								break;
							}
						}
					}

					if (isProcess && (!collectFromList || isListedProcess)) {
						in.open( (std::string("/proc/") + procEntity->d_name + "/stat").c_str() );

						if (in && in.good()) {
							std::string line = "";
							std::getline(in, line);

							if (!line.empty()) {
								lines.push_back( (type + " " + line) );
							}
						}

						if (in) {
							in.close();
						}
					}
				}

				closedir(procDirectory);
			}
		}

		if (lines.size() > 0) {
			std::vector<std::string> cpuList = splitStatLine(lines[0]);

			jobjectArray cpuJavaList = createProcessData(env, cpuList);
			jobjectArray ret = env->NewObjectArray(lines.size(), env->GetObjectClass(cpuJavaList), 0);

			env->SetObjectArrayElement(ret, 0, cpuJavaList);
			env->DeleteLocalRef(cpuJavaList);

			for (int i=1; i < lines.size(); i++) {
				std::vector<std::string> list = splitStatLine(lines[i]);

				/*
				 * Add the CPU stat's to the process stat's
				 */
				if (cpuList.size() >= 3) {
					list.push_back(cpuList[1]);
					list.push_back(cpuList[2]);
				}

				jobjectArray javaList = createProcessData(env, list);
				env->SetObjectArrayElement(ret, i, javaList);
				env->DeleteLocalRef(javaList);
			}

			return ret;
		}

		return NULL;
	}
};
