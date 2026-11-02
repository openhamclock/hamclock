module.exports = {
    dependency: {
        platforms: {
            kepler: {
                "autolink": {
                    "HcEngine": {
                        "libraryName": "libHcEngine.so",
                        "linkDynamic": true,
                        "provider": "application",
                        "components": [],
                        "turbomodules": [
                            "HcEngine"
                        ]
                    }
                }
            },
        },
    },
};