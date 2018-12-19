#!/bin/bash

ARCHIVE_NAME=/tmp/data/data.zip
if [[ -f "${ARCHIVE_NAME}" ]]
then
    unzip "${ARCHIVE_NAME}"
fi

./highloadcup $(ls accounts_*.json)