#!/usr/bin/env python
import os, sys

def countRelevantCommas(line):
    openParensPosStack = []
    openParensPos = 0
    charCounter = 0
    numRelevantCommas = 0
    firstOpenParensIndex = line.find("(")

    for char in line:
        if char == '(':
            openParensPosStack.append(charCounter)

        if char == ')':
            openParensPosStack.pop()

        if char == "," and openParensPosStack[-1] == firstOpenParensIndex:
            numRelevantCommas += 1
        charCounter += 1

    return numRelevantCommas

if __name__ == "__main__":
    filelist = [os.path.join(dp, f) for dp, dn, filenames in os.walk('../../') for f in filenames if os.path.splitext(f)[1] == '.cpp' or os.path.splitext(f)[1] == '.h' ]
    incorrectInstanceCounter = 0

    for file in filelist:    
        f = open(file,"r")
        data = f.read()
        rows = data.split("\n")
        count = 0
        full_data = []
        lineCounter = 1

        tempLine = ""

        for row in rows:
            # Line contains LogPrint or LogPrintf
            if row.find("LogPrint") != -1:
                # Collapse multiple lines into one
                tempLine += row
                if tempLine.count("(") == tempLine.count(")"):
                    havePercents = tempLine.count('%') > 0

                    if havePercents:
                        # This line of code has a format specifier that requires checking number of associated arguments
                        # Determine the number of arguments provided, see if that matches the number of format specifiers
                        # Count the number of commas after the format specifier string.  Check to see if it matches the number of format specifiers.
                        # Assumes quotes are not escaped in the specifier string and there are no percent signs when specifying the debug level.

                        # First, determine the position of the comma after the format specifier section, named commaAfterEndSpecifierStringIndex
                        firstSpecifierIndex = tempLine.find('%')
                        startSpecifierStringIndex = tempLine.rfind('"',firstSpecifierIndex)
                        endSpecifierStringIndex = tempLine.find('"',firstSpecifierIndex)
                        commaAfterEndSpecifierStringIndex = tempLine.find(',',endSpecifierStringIndex)

                        # Count the number of commas after the specifier string
                        line = "(" + tempLine[commaAfterEndSpecifierStringIndex:-1]
                        numCommas = countRelevantCommas(line)

                        # Determine number of extra percents after specifier string
                        numExtraPercents = tempLine.count('%', commaAfterEndSpecifierStringIndex)

                        # Subtract extra from total count.  This is the number of expected specifiers
                        numPercents = tempLine.count('%') - numExtraPercents

                        if numPercents != numCommas:
                            print "Incorrect number of arguments for LogPrint(f) statement found."
                            print(str(file) + ":" + str(lineCounter))
                            print "Line = " + tempLine
                            print("numRelevantCommas = " + str(numCommas) + ", numRelevantPercents = " + str(numPercents))
                            print ""
                            
                            incorrectInstanceCounter += 1

                    # Done with this multiline, clear tempLine
                    #print ""
                    tempLine = ""

            lineCounter +=1

    print("# of incorrect instances: " + str(incorrectInstanceCounter))

    sys.exit(incorrectInstanceCounter)
