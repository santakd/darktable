#!/bin/bash

CDPATH=

PATTERN="[0-9]*"

[ ! -z $1 ] && PATTERN="$(basename $1)"

CLI=darktable-cli
TEST_IMAGES=$PWD/images

TEST_COUNT=0
TEST_ERROR=0
COMPARE=$(which compare)

[ -z $(which $CLI) ] && echo Make sure $CLI is in the path && exit 1

for dir in $(ls -d $PATTERN); do
    echo Test $dir
    TEST_COUNT=$((TEST_COUNT + 1))

    if [ -f $dir/test.sh ]; then
        # The test has a specific driver
        (
            $dir/test.sh
        )

        if [ $? = 0 ]; then
            echo "  OK"
        else
            echo "  FAILS: specific test"
            TEST_ERROR=$((TEST_ERROR + 1))
        fi

    else
        # A standard test
        #   - xmp to create the output
        #   - expected. is the expected output
        #   - a diff is made to compute the max Delta-E
        (
            cd $dir

            # remove leading "????-"

            TEST=${dir:5}

            [ ! -f $TEST.xmp ] &&
                echo missing $dir.xmp && exit 1

            [ ! -f expected.png ] && echo "      missing expected.png"

            IMAGE=$(grep DerivedFrom $TEST.xmp | cut -d'"' -f2)

            echo "      Image $IMAGE"

            # Remove previous output and diff if any

            rm -f output*.png diff*.png

            # Create the output
            #
            # Note that we force host_memory_limit has this will have
            # impact on the tiling and will change the output.
            #
            # This means that the tiling algorithm is probably broken.
            #

            $CLI --width 2048 --height 2048 \
                 --hq true --apply-custom-presets false \
                 "$TEST_IMAGES/$IMAGE" "$TEST.xmp" output.png \
                 --core --disable-opencl \
                 --conf host_memory_limit=8192 2> /dev/null

            res=$?

            $CLI --width 2048 --height 2048 \
                 --hq true --apply-custom-presets false \
                 "$TEST_IMAGES/$IMAGE" "$TEST.xmp" output-cl.png \
                 --core --conf host_memory_limit=8192 2> /dev/null

            res=$((res + $?))

            # If all ok, check Delta-E

            if [ $res -eq 0 ]; then
                if [ ! -z $COMPARE ]; then
                    compare output.png output-cl.png diff-cl.png

                    if [ $? -ne 0 ]; then
                        echo "      CPU & GPU version differs"
                    fi
                fi

                ../deltae expected.png output.png

                if [ $? = 0 ]; then
                    echo "  OK"
                else
                    echo "  FAILS: image visually changed"
                    if [ ! -z $COMPARE ]; then
                        compare expected.png output.png diff.png
                        echo "         see diff.jpg for visual difference"
                    fi
                    exit 1
                fi

            else
                echo "  FAILS : darktable-cli errored"
                exit 1
            fi
        )

        if [ $? -ne 0 ]; then
            TEST_ERROR=$((TEST_ERROR + 1))
        fi
    fi

    echo
done

echo
echo "Total test $TEST_COUNT"
echo "Errors     $TEST_ERROR"
