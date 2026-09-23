#pragma once

#define _USE_MATH_DEFINES
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

class PFM
{	
	static const float getEndianness()
	{
		enum PFM_endianness {
			e_BIG,
			e_LITTLE,
			e_ERROR
		};

		const uint32_t endianness = 0xdeadbeef;
		//std::cout << "\n" << std::bitset<32>(endianness) << std::endl;
		const unsigned char * temp = reinterpret_cast<const unsigned char *>(&endianness);
		//std::cout << std::bitset<8>(*temp) << std::endl;
		PFM_endianness endianType = (((uint32_t)(*temp) ^ 0x0ef) == 0 ? e_LITTLE : ((uint32_t)(*temp) ^ (0x0de)) == 0 ? e_BIG : e_ERROR);

		if (endianType == e_ERROR) {
			std::cerr << "Failed to detect endianness of this system." << std::endl;
			throw;
		}

		return endianType == e_LITTLE ? -1.0f : 1.0f;
	}

	static std::string readNextToken(std::ifstream &ifs)
	{
		std::string retStr;
		retStr.reserve(10);

		for (;;) {
			char c;
			ifs.get(c);
			if (c == '\n' || c == '\r' || c == ' ')
				break;
			retStr += c;
		}

		// delete 0x0A, 0x0D and delimiters 
		for (;;) {
			int c = ifs.peek();
			if (c == std::char_traits<char>::eof())
				break;

			if (c == '\n' || c == '\r' || c == ' ')
				ifs.get();
			else
				break;
		}

		return retStr;
	}

public:

	template<typename T>
	static void writePFM(const std::string & fileName, size_t width, size_t height, const std::vector<T> &imgBuffer)
	{
		std::ofstream ofs;

		try {
			ofs.open(fileName.c_str(), std::ofstream::out | std::ofstream::binary);
		}
		catch (const std::ofstream::failure& e) {
			std::cerr << "Failed to open file for writing a PFM : " << fileName << std::endl;
			throw e;
		}

		ofs << "PF" << std::endl;
		ofs << width << " " << height << std::endl;
		ofs << getEndianness() << std::endl;

		uint32_t inputChannels = imgBuffer.size() / (width * height);

		if (inputChannels < 3)
		{
			std::cerr << "Require at least 3 channels in input data" << std::endl;
			ofs.close();
		}

		if(inputChannels != 3)
		{
			std::vector<T>	temp;
			temp.resize(width * height * 3);	// PFM expects 3 channels

			for (int i = 0; i < width * height; i++) {
				temp[i * 3 + 0] = imgBuffer[i * inputChannels + 0];
				temp[i * 3 + 1] = imgBuffer[i * inputChannels + 1];
				temp[i * 3 + 2] = imgBuffer[i * inputChannels + 2];
			}

			ofs.write((char *)&temp[0], width*height * 3 * sizeof(T));
		}
		else
		{
			ofs.write((char *)&imgBuffer[0], width*height * 3 * sizeof(T));
		}

		ofs.close();
	}

};

