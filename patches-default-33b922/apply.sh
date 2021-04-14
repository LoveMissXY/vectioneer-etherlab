#!/usr/bin/env bash

# if something goes wrong then `set -e` will break this script.
set -e
set -o pipefail # also fail on pipe

BD=$(realpath "$(dirname "${BASH_SOURCE[0]}")")
GITBD=$(git -C ${BD} rev-parse --show-toplevel)

function indent() {
  sed "s/^/${1}/"
}

for SUBDIR in $(ls -d1 ${BD}/*/ | xargs realpath --relative-to=${BD} | grep "^[0-9]\{4\}[^0-9]" | sort); do

    PATCHDIR="${BD}/${SUBDIR}"
    PATCHLIST="${PATCHDIR}/series"
    if [ ! -f "${PATCHLIST}" ]; then
        echo "ERROR could not find ${PATCHLIST}"
        exit 1
    fi
    echo ">> Applying patches in ${SUBDIR}/series"

    # Apply the listed patches in PATCHLIST, if something goes wrong
    # then `set -e` will break this script.
    while read line; do

        [[ "${line}" =~ ^\#.* ]] && continue

        echo "  Apply patch ${line}"

        PATCHFILE="${PATCHDIR}/${line}"

        # Fix wrong user
        FP="^(# User Florian Pose)\$"
        if grep -E -q "${FP}" "${PATCHFILE}"; then
            TMPFILE=$(mktemp)
            cp ${PATCHFILE} ${TMPFILE}
            sed -i -E "s/${FP}/\1 <fp@igh.de>/g" ${TMPFILE}
            PATCHFILE=${TMPFILE}
        fi

        git -C ${GITBD} am -3 ${PATCHFILE} | indent "  - "

        [ -f "${TMPFILE}" ] && rm ${TMPFILE}

    done < ${PATCHLIST}

    echo -e "<< Completed patches from ${SUBDIR}/series\n"
done

exit 0
