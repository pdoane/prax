// The page's side of the web backend of resources (core_web_resource.cpp), which Emscripten links into every program
// that links core (--js-library).

addToLibrary({
    // Returns the path of the script that Node.js runs, as text in memory from malloc, or null on a page, which has
    // no script of its own on a file system.
    prCoreWebResourceScript__deps: ["$prWebNewString"],
    prCoreWebResourceScript: function() {
        const node = typeof process === "object" && process.versions !== undefined &&
            process.versions.node !== undefined;
        return node && process.argv.length > 1 ? prWebNewString(process.argv[1]) : 0;
    },
});
