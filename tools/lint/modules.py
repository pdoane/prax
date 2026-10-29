"""The build's targets as its CMakeLists.txt files declare them: each target's sources, the public headers it owns and
the targets it links. Lint checks includes against this graph.

A public header (pr/<name>.h) is owned by the target whose public include directory holds it. A target's files are its
sources, the internal headers beside its sources in lib/, the headers beside its sources that share their names, and
the public headers that it owns.
"""

import glob
import os
import re

# The ways a target can link another, as CMake spells them.
VISIBILITIES = ("PUBLIC", "PRIVATE", "INTERFACE")


class Target:
    def __init__(self, name):
        self.name = name
        self.kind = None            # "library" or "executable", or None for a name that only links
        self.file = None            # the CMake file that creates the target, relative to the root
        self.line = 0               # where the target is created
        self.sources = []           # paths relative to the root
        self.include = []           # public include directories, relative to the root
        self.links = {}             # by linked target: its visibility
        self.link_lines = {}        # by linked target: the file, relative to the root, and the line that link it


def calls(text):
    """Yields each command of CMake text as (name, arguments, line), with comments removed. An argument keeps a
    generator expression whole and loses its quotes."""
    code = re.sub(r"#[^\n]*", lambda match: " " * len(match.group(0)), text)
    for match in re.finditer(r"\b(\w+)\s*\(", code):
        depth = 1
        end = match.end()
        while depth and end < len(code):
            if code[end] == "(":
                depth += 1
            elif code[end] == ")":
                depth -= 1
            end += 1
        body = code[match.end():end - 1]
        line = code.count("\n", 0, match.start()) + 1
        arguments = []
        for argument in re.finditer(r'"[^"]*"|\$<[^>]*>|[^\s]+', body):
            value = argument.group(0).strip('"')
            arguments.append((value, line + code.count("\n", match.end(), match.end() + argument.start())))
        yield match.group(1).lower(), arguments, line


# The extensions of the files that a target compiles or a directory holds, which a bare argument is a path if it has.
SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".mm", ".h")


def relative(value, directory):
    """Returns a path argument of a CMake file in `directory` relative to the root, or None if it is not a path in the
    source tree. `directory` is relative to the root."""
    match = re.match(r"\$<BUILD_INTERFACE:(.*)>$", value)
    if match:
        value = match.group(1)
    if value.startswith("${CMAKE_CURRENT_SOURCE_DIR}/"):
        value = os.path.join(directory, value[len("${CMAKE_CURRENT_SOURCE_DIR}/"):])
    elif value.startswith("${PROJECT_SOURCE_DIR}/"):
        value = value[len("${PROJECT_SOURCE_DIR}/"):]
    elif "${" in value or "$<" in value or ("/" not in value and not value.endswith(SOURCE_EXTENSIONS)):
        return None
    else:
        value = os.path.join(directory, value)
    return os.path.normpath(value).replace(os.sep, "/")


def commands(root, path="CMakeLists.txt"):
    """Yields each command of the build as (file, name, arguments, line), the file relative to the root, in the order
    that CMake reads them: a directory's commands, with each included file and added directory in its place."""
    full = os.path.join(root, path)
    if not os.path.exists(full):
        return
    with open(full) as file:
        text = file.read()
    directory = os.path.dirname(path)
    for command, arguments, line in calls(text):
        if command == "add_subdirectory" and arguments and "${" not in arguments[0][0]:
            yield from commands(root, os.path.normpath(os.path.join(directory, arguments[0][0], "CMakeLists.txt")))
        elif command == "include" and arguments and arguments[0][0].endswith(".cmake"):
            yield from commands(root, os.path.normpath(os.path.join(directory, arguments[0][0])))
        else:
            yield path.replace(os.sep, "/"), command, arguments, line


def targets(root):
    """Returns the targets of the build that root/CMakeLists.txt describes, by name, or an empty mapping if the file is
    missing. Every branch of a condition is read, so a target declared for one platform is a target on every platform.
    A library of the engine (pr_library) is named by its module's name, as prax::<name> names it. Another target whose
    CMake name is a module's, as an example may be, is a target of its own, named apart from the module."""
    found = {}
    every = list(commands(root))
    libraries = {arguments[0][0] for _, command, arguments, _ in every if command == "pr_library" and arguments}

    def name_of(value):
        """Returns the target that `value`, a CMake name, names: a library of the engine by its module's name, as
        prax::<name> or pr_<name>. A bare module name is CMake's name for another target, which keeps its name apart
        from the module's."""
        if value.startswith("prax::"):
            return value[len("prax::"):]
        if value.startswith("pr_") and value[len("pr_"):] in libraries:
            return value[len("pr_"):]
        if value in libraries:
            return value + " (target)"
        return value

    def target(name):
        name = name_of(name)
        return found.setdefault(name, Target(name))

    def module(name):
        """Returns the library of the module `name`, which pr_library and pr_module_tests name bare."""
        return found.setdefault(name, Target(name))

    def create(name, kind, path, line, sources, directory, library=False):
        created = module(name) if library else target(name)
        created.kind = kind
        created.file = path
        created.line = line
        created.sources += [source for source in (relative(value, directory) for value, _ in sources) if source]
        return created

    def link(name, values, path, visibility="PRIVATE"):
        for value, line in values:
            if value in VISIBILITIES:
                visibility = value
                continue
            match = re.match(r"\$<(?:COMPILE_ONLY|BUILD_LOCAL_INTERFACE):([\w:]+)>$", value)
            if match:
                value = match.group(1)
            if re.match(r"[\w:]+$", value):
                linked = name_of(value)
                target(name).links.setdefault(linked, visibility)
                target(name).link_lines.setdefault(linked, (path, line))

    for path, command, arguments, line in every:
        if not arguments or "${" in arguments[0][0]:
            continue
        directory = os.path.dirname(path)
        name = arguments[0][0]
        rest = arguments[1:]
        if command in ("add_library", "add_executable"):
            if any(value == "ALIAS" for value, _ in rest):
                continue
            create(name, "library" if command == "add_library" else "executable", path, line, rest, directory)
        elif command == "pr_library":
            values = [value for value, _ in rest]
            sources = rest
            if "INCLUDE" in values:
                index = values.index("INCLUDE")
                include = relative(values[index + 1] + "/", directory) if index + 1 < len(values) else None
                sources = rest[:index] + rest[index + 2:]
                created = create(name, "library", path, line, sources, directory, True)
                if include:
                    created.include.append(include)
            else:
                create(name, "library", path, line, sources, directory, True)
        elif command == "pr_headers" and len(rest) >= 2:
            include = relative(rest[1][0] + "/", directory)
            if include:
                target(name).include.append(include)
        elif command == "target_sources":
            target(name).sources += [source for source in (relative(value, directory) for value, _ in rest) if source]
        elif command == "target_include_directories":
            visibility = None
            for value, _ in rest:
                if value in VISIBILITIES:
                    visibility = value
                elif visibility in ("PUBLIC", "INTERFACE") and relative(value + "/", directory):
                    target(name).include.append(relative(value + "/", directory))
        elif command == "target_link_libraries":
            link(name, rest, path)
        elif command == "pr_module_tests":
            # A module's test binary links the module, the toolkit and doctest.
            create(name + "_tests", "executable", path, line, rest, directory)
            link(name + "_tests", [("prax::" + name, line), ("prax::core_cpp", line)], path)
        elif command == "pr_net_tests":
            # A test binary of the network modules links their test servers' support and the platform's sockets.
            create(name, "executable", path, line, rest, directory)
            link(name, [("net_peer", line), ("prax::net", line), ("prax::core_cpp", line)], path)
        elif command in ("pr_harness", "pr_example") and rest:
            # A harness links the module it exercises and the harness support. An example links what it names.
            if command == "pr_harness":
                create(name, "executable", path, line, rest[1:], directory)
                link(name, [(rest[0][0], line), ("harness", line)], path)
            else:
                create(name, "executable", path, line, rest, directory)
    return found


def header_owners(root, graph):
    """Returns the target that owns each public header, by the header's name under pr/. The first target to claim a
    directory owns its headers, as a configured copy of a library shares the library's headers."""
    owners = {}
    for target in graph.values():
        for directory in target.include:
            for header in glob.glob(os.path.join(root, directory, "pr", "*.h")):
                owners.setdefault(os.path.basename(header), target.name)
    return owners


def source_owners(graph):
    """Returns the target that owns each source directory under lib/, by the directory relative to lib/, so that an
    internal include (<module>/src/<name>.h) names its target. The first target to list a directory owns it."""
    owners = {}
    for target in graph.values():
        if target.kind != "library":
            continue
        for source in target.sources:
            if source.startswith("lib/") and "/tests/" not in source:
                owners.setdefault(os.path.dirname(source)[len("lib/"):], target.name)
    return owners


def files(root, target):
    """Returns the absolute paths of the target's files."""
    found = set()
    for source in target.sources:
        path = os.path.join(root, source)
        if not os.path.exists(path):
            continue
        found.add(path)
        directory = os.path.dirname(path)
        if source.startswith("lib/"):
            found.update(glob.glob(os.path.join(directory, "*.h")))
        else:
            header = os.path.splitext(path)[0] + ".h"
            if os.path.exists(header):
                found.add(header)
    for directory in target.include:
        found.update(glob.glob(os.path.join(root, directory, "pr", "*.h")))
    return found
