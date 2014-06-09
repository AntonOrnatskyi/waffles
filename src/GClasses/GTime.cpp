/*
  The contents of this file are dedicated by all of its authors, including

    Michael S. Gashler,
    Eric Moyer,
    anonymous contributors,

  to the public domain (http://creativecommons.org/publicdomain/zero/1.0/).

  Note that some moral obligations still exist in the absence of legal ones.
  For example, it would still be dishonest to deliberately misrepresent the
  origin of a work. Although we impose no legal requirements to obtain a
  license, it is beseeming for those who build on the works of others to
  give back useful improvements, or find a way to pay it forward. If
  you would like to cite us, a published paper about Waffles can be found
  at http://jmlr.org/papers/volume12/gashler11a/gashler11a.pdf. If you find
  our code to be useful, the Waffles team would love to hear how you use it.
*/

#include <stdio.h>
#include "GTime.h"
#include "GString.h"
#include <time.h>
#ifdef WINDOWS
#	include <windows.h>
#else
#	include <sys/time.h>
#endif
#include <sstream>

namespace GClasses {

/*static*/ double GTime::seconds()
{
#ifdef WINDOWS
	return (double)GetTickCount() * 1e-3;
#else
	struct timeval tp;
	gettimeofday(&tp, NULL);
	return ((double)tp.tv_sec + (double)tp.tv_usec * 1e-6);
#endif
}

/*static*/ const char* GTime::asciiTime(char* szBuf, int nSize, bool bGreenwichMeanTime)
{
	time_t t = time((time_t*)0);
#ifdef WINDOWS
	struct tm thetime;
	if(bGreenwichMeanTime)
		gmtime_s(&thetime, &t);
	else
		localtime_s(&thetime, &t);
	asctime_s(szBuf, nSize, &thetime);
	int len = (int)strlen(szBuf);
#else
	struct tm* pTime = bGreenwichMeanTime ? gmtime(&t) : localtime(&t);
	int len = safe_strcpy(szBuf, asctime(pTime), nSize);
#endif

	// Remove trailing whitespace (cuz asctime appends a '\n' at the end)
	while(len > 0 && szBuf[len - 1] <= ' ')
		szBuf[--len] = '\0';
	return szBuf;
}

void GTime_printTwoDigits(std::ostream& os, unsigned int n)
{
	if(n < 10)
		os << "0";
	os << n;
}

/*static*/ void GTime::appendTimeStampValue(std::string* pS, const char* sep1, const char* sep2, const char* sep3, bool bGreenwichMeanTime)
{
	time_t t = time((time_t*)0);
#ifdef WINDOWS
	struct tm thetime;
	if(bGreenwichMeanTime)
		gmtime_s(&thetime, &t);
	else
		localtime_s(&thetime, &t);
	struct tm* pTime = &thetime;
#else
	struct tm* pTime = bGreenwichMeanTime ? gmtime(&t) : localtime(&t);
#endif
	std::ostringstream os;
	unsigned int n = 1900 + pTime->tm_year;
	os << n;
	os << sep1;
	GTime_printTwoDigits(os, pTime->tm_mon + 1);
	os << sep1;
	GTime_printTwoDigits(os, pTime->tm_mday);
	os << sep2;
	GTime_printTwoDigits(os, pTime->tm_hour);
	os << sep3;
	GTime_printTwoDigits(os, pTime->tm_min);
	os << sep3;
	GTime_printTwoDigits(os, pTime->tm_sec);
	(*pS).append(os.str());
}

int GTime_parseVal(const char* buf, int min, int max, bool* pOk)
{
	for(size_t i = 0; buf[i] != '\0'; i++)
	{
		if(buf[i] < '0' || buf[i] > '9')
			*pOk = false;
	}
	int val = atoi(buf);
	if(val < min || val > max)
		*pOk = false;
	return val;
}

/*static*/ bool GTime::fromString(time_t* pOutTime, const char* szData, const char* szFormat)
{
	struct tm ts;
	ts.tm_sec = 0;
	ts.tm_min = 0;
	ts.tm_hour = 0;
	ts.tm_mday = 1;
	ts.tm_mon = 0;
	ts.tm_year = 70;
	ts.tm_wday = 0;
	ts.tm_yday = 0;
	ts.tm_isdst = 0;
	char buf[32];
	while(szFormat)
	{
		size_t i;
		for(i = 0; i < 30; i++)
		{
			if(szFormat[i] != szFormat[0])
				break;
			if(szData[i] == '\0')
				return false;
			buf[i] = szData[i];
		}
		buf[i] = '\0';
		bool ok = true;
		switch(szFormat[0])
		{
			case 'Y':
				ts.tm_year = GTime_parseVal(buf, 1000, 3000, &ok) - 1900;
				break;
			case 'M':
				ts.tm_mon = GTime_parseVal(buf, 1, 12, &ok) - 1;
				break;
			case 'D':
				ts.tm_mday = GTime_parseVal(buf, 1, 31, &ok);
				break;
			case 'h':
				ts.tm_hour = GTime_parseVal(buf, 0, 23, &ok);
				break;
			case 'm':
				ts.tm_min = GTime_parseVal(buf, 0, 59, &ok);
				break;
			case 's':
				ts.tm_sec = GTime_parseVal(buf, 0, 61, &ok);
				break;
			default:
				for(size_t j = 0; j < i; j++)
				{
					if(buf[j] != szFormat[0])
						return false;
				}
				break;
		}
		if(!ok)
			return false;
		szData += i;
		szFormat += i;
	}
	return mktime(&ts);
}

} // namespace GClasses

