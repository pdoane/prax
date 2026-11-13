#!/usr/bin/env python3
"""Regression tests for lint's file discovery and convention rules.

Usage: python3 -m tools.lint.test_lint
"""

import contextlib
import io
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.lint import layout
from tools.lint import lint
from tools.lint import rewrap


class Discovery(unittest.TestCase):
    def test_checks_flat_and_nested_modules(self):
        for module in ('core', 'url', 'future/module'):
            with self.subTest(module=module), tempfile.TemporaryDirectory() as root:
                # Every lint category must reach the nested module.
                tag = module.split('/')[-1]
                public = f'lib/{module}/include/pr/{tag}_api.h'
                headers = [public,
                           f'lib/{module}/src/runtime.h',
                           f'lib/{module}/tests/support.h']
                sources = [f'lib/{module}/src/runtime.cpp',
                           f'lib/{module}/tests/harness.cpp']
                bindings = [f'lib/{module}/quickjs/src/api.h', f'lib/{module}/quickjs/src/api.cpp']
                files = {path: '// Missing pragma once.\n' for path in headers}
                files[public] = ('// Missing pragma once.\nPR_ASSUME_NONNULL_BEGIN\n'
                                 f'typedef struct Pr{tag.capitalize()}Api Pr{tag.capitalize()}Api;\n'
                                 'PR_ASSUME_NONNULL_END\n')
                files.update({path: 'namespace pr_impl {\nvoid run() { new Object; }\n}\n'
                              for path in sources + bindings})
                # A unit test is checked as every other source is.
                files[f'lib/{module}/quickjs/src/api.h'] = (
                    '#pragma once\nPR_ASSUME_NONNULL_BEGIN\n'
                    'namespace pr_impl {\n// Allocates.\nvoid jsRun() { new Object; }\n}\n'
                    'PR_ASSUME_NONNULL_END\n')
                files[f'lib/{module}/tests/test_runtime.cpp'] = 'new Object;\n'
                files['tools/shell/shell.cpp'] = 'int main() { return 0; }\n'
                for path, text in files.items():
                    target = Path(root, path)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(text)
                # The report that main prints, which holds what every file's checks found.
                printed = []
                with patch.object(lint, 'ROOT', root), \
                     patch.object(lint.Report, 'print', autospec=True,
                                  side_effect=lambda report: printed.append(report) or 1), \
                     patch.object(lint, 'spec_ids', return_value={}), \
                     patch.object(lint.subprocess, 'run',
                                  return_value=SimpleNamespace(stdout='')), \
                     patch('sys.argv', ['lint.py']):
                    self.assertEqual(lint.main(), 1)
                self.assertCountEqual([(path, rule) for path, _, rule, _ in printed[0].problems],
                                      [(path, 'pragma-once') for path in headers]
                                      + [(path, 'allocation') for path in sources + bindings
                                         + [f'lib/{module}/tests/test_runtime.cpp']])


class BindingConventions(unittest.TestCase):
    def test_multiple_anonymous_namespaces_are_rejected(self):
        report = lint.Report()
        lint.check_anonymous_namespace(report, os.path.join(lint.ROOT, 'lib/webidl/quickjs_cpp/src/webidl_quickjs.cpp'),
                                       'namespace {\n}\nnamespace {\n}\n')
        self.assertEqual([rule for _, _, rule, _ in report.problems], ['anonymous-namespace'])


class RedundantCasts(unittest.TestCase):
    ENUMS = "enum {\n    Flag_A = 1,\n    Flag_B = 2,\n};\ntypedef uint8_t Flags;\nenum {\n    Kind_X,\n};\ntypedef uint8_t Kind;\n"

    def rules(self, source):
        report = lint.Report()
        text = self.ENUMS + source
        lint.check_redundant_casts(report, os.path.join(lint.ROOT, "lib/core/src/a.cpp"), text,
                                   lint.enum_constants([text]))
        return [rule for _, _, rule, _ in report.problems]

    def test_a_constant_converts_except_as_a_ternary_arm(self):
        self.assertEqual(self.rules("Kind k = Kind(Kind_X);\n"), ["redundant-cast"])
        self.assertEqual(self.rules("S s{.k = Kind(Kind_X)};\n"), ["redundant-cast"])
        self.assertEqual(self.rules("S s{.k = c ? k : Kind(Kind_X)};\n"), [])

    def test_a_combination_of_flags_converts_outside_a_braced_initializer(self):
        self.assertEqual(self.rules("set(Flags(flags | Flag_A));\n"), ["redundant-cast"])
        self.assertEqual(self.rules("S s{.f = Flags(flags | Flag_A)};\n"), [])
        self.assertEqual(self.rules("set(Flags(~flags | Flag_A));\n"), [])


class CompilerAttributes(unittest.TestCase):
    def rules(self, source, path="lib/core/src/a.cpp"):
        report = lint.Report()
        lint.check_compiler_attributes(report, os.path.join(lint.ROOT, path), source)
        return [rule for _, _, rule, _ in report.problems]

    def test_an_attribute_of_a_compiler_is_spelled_through_a_macro(self):
        self.assertEqual(self.rules("[[gnu::noinline]] void f();\n"), ["compiler-attribute"])
        self.assertEqual(self.rules("void f() __attribute__((cold));\n"), ["compiler-attribute"])
        self.assertEqual(self.rules("__declspec(noinline) void f();\n"), ["compiler-attribute"])
        self.assertEqual(self.rules("[[ clang::musttail ]] return f();\n"), ["compiler-attribute"])
        self.assertEqual(self.rules("PR_NOINLINE void f();\n[[nodiscard]] int g();\n"), [])
        self.assertEqual(self.rules("// [[gnu::noinline]] in a comment\n"), [])

    def test_core_spells_the_macros(self):
        self.assertEqual(self.rules("#define PR_COLD __attribute__((cold))\n", lint.ATTRIBUTE_HEADER), [])


class ManagedLayer(unittest.TestCase):
    def rules(self, path, source):
        report = lint.Report()
        full = os.path.join(lint.ROOT, path)
        lint.check_destructors(report, full, source)
        lint.check_managed_includes(report, full, source)
        return [rule for _, _, rule, _ in report.problems]

    def test_an_implementation_declares_no_destructor(self):
        self.assertEqual(self.rules("lib/font/src/a.cpp", "struct A {\n    ~A() { f(); }\n};\n"), ["destructor"])
        self.assertEqual(self.rules("lib/font/src/a.cpp", "Owner::~Owner() {}\n"), ["destructor"])
        self.assertEqual(self.rules("lib/font/src/a.cpp", "    virtual ~Base() = default;\n"), ["destructor"])

    def test_bitwise_not_and_comments_are_not_destructors(self):
        self.assertEqual(self.rules("lib/font/src/a.cpp", "    x = ~mask();\n    // ~A() in a comment\n"), [])

    def test_tests_and_applications_may_destroy(self):
        self.assertEqual(self.rules("lib/font/tests/test_a.cpp", "struct A {\n    ~A() {}\n};\n"), [])
        self.assertEqual(self.rules("examples/a/a.cpp", "struct A {\n    ~A() {}\n};\n"), [])

    def test_only_applications_tests_and_the_layer_include_its_headers(self):
        with patch.object(lint, "MANAGED", {"pr/array.h"}):
            self.assertEqual(self.rules("lib/font/src/a.cpp", '#include "pr/array.h"\n'), ["managed-include"])
            self.assertEqual(self.rules("lib/font/tests/test_a.cpp", '#include "pr/array.h"\n'), [])
            self.assertEqual(self.rules("examples/a/a.cpp", '#include "pr/array.h"\n'), [])
            self.assertEqual(self.rules("lib/core/cpp/include/pr/string.h",
                                        '#include "pr/array.h"\n'), ["managed-include"])
            self.assertEqual(self.rules("lib/core/cpp/include/pr/array.h", "    ~Array() { reset(); }\n"), [])


class ApplicationIncludes(unittest.TestCase):
    def rules(self, path, source):
        report = lint.Report()
        lint.check_application_includes(report, os.path.join(lint.ROOT, path), source)
        return [rule for _, _, rule, _ in report.problems]

    def test_an_application_includes_no_other_applications_headers(self):
        self.assertEqual(self.rules("apps/merge/main.cpp", '#include "apps/merge/merge.h"\n'), [])
        self.assertEqual(self.rules("apps/merge/main.cpp", '#include "apps/scribe/scribe.h"\n'),
                         ["application-include"])
        self.assertEqual(self.rules("examples/a/a.cpp", '#include "apps/scribe/scribe.h"\n'), ["application-include"])
        self.assertEqual(self.rules("apps/merge/main.cpp", '#include "examples/editor/hud.h"\n'),
                         ["application-include"])
        self.assertEqual(self.rules("tools/bench/a.cpp", '#include "examples/draw_features/drawing.h"\n'), [])


class TestIncludes(unittest.TestCase):
    def rules(self, path, source):
        report = lint.Report()
        lint.check_test_include(report, os.path.join(lint.ROOT, path), source)
        return [rule for _, _, rule, _ in report.problems]

    def test_a_test_includes_doctest_through_the_harness(self):
        self.assertEqual(self.rules("lib/font/tests/test_a.cpp", '#include "doctest/doctest.h"\n'), ["test-include"])
        self.assertEqual(self.rules("tools/harness/kit_fixture.h", '#include "doctest/doctest.h"\n'), ["test-include"])
        self.assertEqual(self.rules("lib/font/tests/test_a.cpp", '#include "tools/harness/test.h"\n'), [])

    def test_the_harness_includes_doctest_itself(self):
        self.assertEqual(self.rules("tools/harness/test.h", '#include "doctest/doctest.h"\n'), [])
        self.assertEqual(self.rules("tools/harness/test_main.cpp", '#include "doctest/doctest.h"\n'), [])


class Builtins(unittest.TestCase):
    def rules(self, source, path="lib/core/src/a.cpp"):
        report = lint.Report()
        lint.check_builtins(report, os.path.join(lint.ROOT, path), source)
        return [rule for _, _, rule, _ in report.problems]

    def test_a_builtin_is_reached_through_core(self):
        self.assertEqual(self.rules("return __builtin_ctzll(word);\n"), ["builtin"])
        self.assertEqual(self.rules("return __builtin_ctzll(word);\n", "lib/core/include/pr/integer.h"), [])

    def test_a_type_intrinsic_is_reached_through_type_traits(self):
        self.assertEqual(self.rules("static_assert(__is_trivially_copyable(T));\n"), ["builtin"])
        self.assertEqual(self.rules("static_assert(__is_trivially_copyable(T));\n",
                                    "lib/core/include/pr/integer.h"), ["builtin"])
        self.assertEqual(self.rules("bool b = __is_empty(T);\n", lint.TYPE_TRAITS), [])
        self.assertEqual(self.rules("// __is_empty(T) in a comment\nint __is_x = 0;\n"), [])


class ContainerViews(unittest.TestCase):
    def rules(self, source):
        report = lint.Report()
        lint.check_container_views(report, os.path.join(lint.ROOT, "examples/a/a.cpp"), source)
        return [rule for _, _, rule, _ in report.problems]

    def test_a_view_of_a_whole_container_is_the_container(self):
        self.assertEqual(self.rules("f(PrStringView{url.data(), url.size()});\n"), ["container-view"])
        self.assertEqual(self.rules("return PrStringView(text.data(), text.size());\n"), ["container-view"])
        self.assertEqual(self.rules("g(PrStringView{tab->info.url.data(), tab->info.url.size()});\n"),
                         ["container-view"])
        self.assertEqual(self.rules("g(PrStringView{items[i].url.data(), items[i].url.size()});\n"),
                         ["container-view"])
        self.assertEqual(self.rules("decode(PrByteView{body.data(), body.size()});\n"), ["container-view"])

    def test_a_part_of_a_container_or_of_two_keeps_its_length(self):
        self.assertEqual(self.rules("f(PrStringView{file.data(), slash});\n"), [])
        self.assertEqual(self.rules("f(PrStringView{a.data(), b.size()});\n"), [])
        self.assertEqual(self.rules("f(PrStringView{text.data() + at, text.size() - at});\n"), [])
        self.assertEqual(self.rules("// PrStringView{url.data(), url.size()} in a comment\n"), [])


class LiteralViews(unittest.TestCase):
    def rules(self, source):
        report = lint.Report()
        lint.check_literal_views(report, os.path.join(lint.ROOT, "lib/core/src/a.cpp"), source)
        return [rule for _, _, rule, _ in report.problems]

    def test_a_view_of_a_whole_literal_is_the_literal(self):
        self.assertEqual(self.rules('PrStringView v = PrStringView{"abc", 3};\n'), ["literal-view"])
        self.assertEqual(self.rules('constexpr PrStringView kName = {"#000000", 7};\n'), ["literal-view"])
        self.assertEqual(self.rules('PrStringView text{"", 0};\n'), ["literal-view"])
        self.assertEqual(self.rules('f(PrStringView{"\\xe2\\x88\\x9e px", 6});\n'), ["literal-view"])

    def test_a_literal_that_holds_a_nul_or_a_part_of_a_literal_keeps_its_length(self):
        self.assertEqual(self.rules('f(PrStringView{"<\\0?\\0", 4});\n'), [])
        self.assertEqual(self.rules('f(PrStringView{"abcdef", 3});\n'), [])
        self.assertEqual(self.rules('f(PrStringView{&letter, 1});\n'), [])

    def test_a_literal_converts_where_a_view_is_expected(self):
        self.assertEqual(self.rules('f(PrStringView{"abc"});\n'), ["literal-view"])
        self.assertEqual(self.rules('event.text = PrStringView{""};\n'), ["literal-view"])
        self.assertEqual(self.rules('CHECK(view == PrStringView{"red"});\n'), ["literal-view"])
        self.assertEqual(self.rules('return PrStringView{""};\n'), ["literal-view"])

    def test_a_view_that_a_ternary_or_its_use_needs_names_its_type(self):
        self.assertEqual(self.rules('v = ok ? text : PrStringView{""};\n'), [])
        self.assertEqual(self.rules('v = ok ? PrStringView{"a"} : text;\n'), [])
        self.assertEqual(self.rules('CHECK(PrStringView("color") == "color");\n'), [])
        self.assertEqual(self.rules('n = PrStringView("color").size;\nfor (char c : PrStringView("abc")) {}\n'), [])


class Auto(unittest.TestCase):
    def rules(self, source):
        report = lint.Report()
        lint.check_auto(report, os.path.join(lint.ROOT, "lib/core/src/a.cpp"), source)
        return [rule for _, _, rule, _ in report.problems]

    def test_a_declaration_names_its_type(self):
        self.assertEqual(self.rules("auto* p = static_cast<T*>(q);\n"), ["auto"])
        self.assertEqual(self.rules("for (const auto& x : xs) {}\n"), ["auto"])
        self.assertEqual(self.rules("// auto in a comment\nconst char* s = \"auto\";\n"), [])

    def test_what_has_no_name_to_write_is_auto(self):
        self.assertEqual(self.rules("auto f = [&](int x) { return x; };\n"), [])
        self.assertEqual(self.rules("auto [a, b] = pair;\nconst auto& [c, d] = pair;\n"), [])
        self.assertEqual(self.rules("each([&](auto visit, const auto& x) { visit(x); });\n"), [])
        self.assertEqual(self.rules("template <const int* Id, auto Get>\nint f();\n"), [])
        self.assertEqual(self.rules("template <typename T>\nauto f(T t) { return t; }\n"), ["auto"])


class StandardLibrary(unittest.TestCase):
    def rules(self, path, source):
        report = lint.Report()
        lint.check_stdlib(report, os.path.join(lint.ROOT, path), source)
        return [(line, rule) for _, line, rule, _ in report.problems]

    def test_a_wrapper_includes_its_facility(self):
        self.assertEqual(self.rules('lib/core/backends/posix/src/core_posix_thread.cpp', '#include <pthread.h>\n'), [])
        self.assertEqual(self.rules('lib/core/src/vec.cpp', '#include <pthread.h>\n'), [(1, 'stdlib')])

    def test_the_tests_and_the_harness_headers_are_held_to_it(self):
        self.assertTrue(lint.is_on_core(os.path.join(lint.ROOT, 'tools/harness/test.h')))
        self.assertFalse(lint.is_on_core(os.path.join(lint.ROOT, 'tools/harness/harness.cpp')))
        self.assertFalse(lint.is_on_core(os.path.join(lint.ROOT, 'tools/viewer/viewer.cpp')))
        self.assertEqual(self.rules('lib/core/tests/test_string_ops.cpp', '#include <string>\n'), [(1, 'stdlib')])
        self.assertEqual(self.rules('lib/process/tests/test_process.cpp', '#include <sys/resource.h>\n'), [])

    def test_a_public_header_wraps_nothing(self):
        path = 'lib/core/include/pr/neon.h'
        with patch.dict(lint.WRAPPERS, {path: {'arm_neon.h'}}):
            self.assertEqual(self.rules(path, '#include <stdint.h>\n#include <arm_neon.h>\n'),
                             [(1, 'stdlib'), (2, 'stdlib')])


class Namespaces(unittest.TestCase):
    def rules(self, check, path, source, *args):
        report = lint.Report()
        check(report, os.path.join(lint.ROOT, path), source, *args)
        return [(line, rule) for _, line, rule, _ in report.problems]

    def test_pr_impl_is_spelled_out_only_by_a_macro(self):
        source = ('#define ASSERT(c) \\\n    ::pr_impl::fail(c)\n'
                  '// pr_impl::inComment\n'
                  'const char* text = "pr_impl::inLiteral";\n'
                  'pr_impl::Value value;\n')
        self.assertEqual(self.rules(lint.check_qualified, 'examples/viewer/a.cpp', source),
                         [(5, 'qualified')])

    def test_pr_is_spelled_out_only_in_an_explicit_specialization(self):
        source = ('namespace doctest {\n'
                  'template <>\n'
                  'struct StringMaker<pr::String> {\n'
                  '    static String convert(const pr::String& text);\n'
                  '};\n'
                  'template <> struct Hash<pr::Span<int>>;\n'
                  'template <>\n'
                  'struct StringMaker<pr_impl::Value> {};\n'
                  '} // namespace doctest\n'
                  'pr::String text;\n'
                  'template <typename T>\n'
                  'struct Holder { pr::Array<T> items; };\n')
        self.assertEqual(self.rules(lint.check_qualified, 'tools/harness/a.h', source),
                         [(8, 'qualified'), (10, 'qualified'), (12, 'qualified')])

    def test_an_application_uses_pr_and_not_pr_impl(self):
        source = ('// pr_impl in a comment\nusing namespace pr;\nusing namespace pr_impl;\n'
                  'namespace viewer {\nusing pr::Value;\n}\nnamespace pr_impl {\n}\n')
        self.assertEqual(self.rules(lint.check_application_namespace, 'examples/viewer/a.h', source),
                         [(3, 'application-namespace'), (7, 'application-namespace')])

    def test_a_c_name_has_the_prefix_and_global_scope(self):
        source = ('extern "C" int prTestCApi(void);\nextern "C" int testCApi(void);\n'
                  'extern "C" void _ReadWriteBarrier(void);\n'
                  'namespace pr_impl {\nextern "C" int prTestInside(void);\n}\n')
        self.assertEqual(self.rules(lint.check_linkage, 'lib/core/tests/test_a.cpp', source),
                         [(2, 'linkage'), (5, 'linkage')])
        self.assertEqual(self.rules(lint.check_internal_names, 'lib/core/tests/test_a.cpp', source[:33], set()), [])

    def test_a_header_takes_no_single_name_at_global_scope(self):
        source = 'using pr_impl::Value;\n'
        self.assertEqual(self.rules(lint.check_qualified, 'lib/core/tests/a.h', source), [(1, 'qualified')])

    def test_a_test_case_is_at_global_scope(self):
        source = ('namespace pr_impl {\n\nnamespace {\n\nTEST_CASE("hidden") {}\n\n} // namespace\n\n'
                  '} // namespace pr_impl\n\nusing namespace pr_impl;\n\nTEST_CASE("found") {}\n')
        self.assertEqual(self.rules(lint.check_test_cases, 'lib/core/tests/test_a.cpp', source),
                         [(5, 'test-case')])

    def test_a_test_constant_may_be_const(self):
        source = 'const std::vector<int> kRows = {1, 2};\n'
        self.assertEqual(self.rules(lint.check_constant_names, 'lib/core/tests/test_a.cpp', source), [])
        self.assertEqual(self.rules(lint.check_constant_names, 'lib/core/src/a.cpp', source),
                         [(1, 'constant-form')])

    def test_a_parameter_is_not_named_self(self):
        for source in ('void a(PrNode* self);\n', 'void a(int x, const PrNode* PR_NULLABLE self, int y) {}\n',
                       'auto a = [](JSValueConst self) {};\n', 'void a(JSContext* ctx, JSValueConst thisValue);\n',
'void a(int x,\n    PrNode* self);\n',
                       'void a(void (*visit)(PrNode* self));\n', 'void a(bool self = false);\n'):
            self.assertEqual([rule for _, rule in self.rules(lint.check_self_parameter, 'lib/core/src/a.cpp', source)],
                             ['self-parameter'], source)
        for source in ('[a send:self];\n', 'call(context, self);\n', 'call(a, &self);\n', 'call((__bridge void*)self);\n',
                       'a.delegate = self;\n', 'PrLayoutAlignment self = a.alignSelf;\n', 'bool self;\n',
                       'char c[] = "(int self)"; // (int self)\n'):
            self.assertEqual(self.rules(lint.check_self_parameter, 'lib/core/src/a.mm', source), [], source)
        for source in ('    PrPage* self = static_cast<PrPage*>(context);\n', 'const PrNode* self = a;\n'):
            self.assertEqual([rule for _, rule in self.rules(lint.check_self_parameter, 'lib/core/src/a.cpp', source)],
                             ['self-parameter'], source)

    def test_a_header_names_the_base_of_an_opaque_type(self):
        source = 'struct PrValue : pr_impl::Value {};\n'
        self.assertEqual(self.rules(lint.check_qualified, 'lib/core/src/a.h', source), [])
        self.assertEqual(self.rules(lint.check_qualified, 'lib/core/src/a.cpp', source),
                         [(1, 'qualified')])

    def test_a_header_has_one_pr_impl_block_and_then_opaque_types(self):
        good = ('#pragma once\n\nnamespace pr_impl {\n\nstruct Value {};\n\n} // namespace pr_impl\n\n'
                '// C opaque types\nstruct PrValue : pr_impl::Value {};\n\nPR_ASSUME_NONNULL_END\n')
        self.assertEqual(self.rules(lint.check_header_namespace_layout, 'lib/core/src/a.h', good, 'pr_impl'), [])
        bad = ('namespace pr_impl {\nstruct Base {};\n} // namespace pr_impl\n\n'
               'struct PrBase : pr_impl::Base {};\n\nnamespace pr_impl {\nstruct Derived : PrBase {};\n'
               '} // namespace pr_impl\n\ninline void stray() {}\n')
        self.assertEqual(self.rules(lint.check_header_namespace_layout, 'lib/core/src/b.h', bad, 'pr_impl'),
                         [(7, 'namespace-layout'), (11, 'namespace-layout')])

    def test_a_macro_body_may_follow_the_pr_impl_block(self):
        source = ('namespace pr_impl {\nstruct Site {};\n} // namespace pr_impl\n\n'
                  '#define PR_SITE()                    \\\n'
                  '    []() { static constexpr ::pr_impl::Site site = {}; \\\n'
                  '        return &site; }()\n\ninline void stray() {}\n')
        self.assertEqual(self.rules(lint.check_header_namespace_layout, 'lib/core/src/a.h', source,
                                    'pr_impl'),
                         [(9, 'namespace-layout')])

    def test_what_reaches_the_linker_has_a_prefix_or_a_namespace(self):
        source = ('using namespace pr_impl;\n\nnamespace {\n\nvoid local() {}\n\n} // namespace\n\n'
                  'namespace pr_impl {\n\nvoid internal() {}\n\n} // namespace pr_impl\n\n'
                  'void prPublic() {}\n\nvoid stray() {}\n')
        self.assertEqual(self.rules(lint.check_global_definitions, 'lib/core/src/a.cpp', source),
                         [(17, 'global')])

    def test_a_source_is_one_pr_impl_block_then_its_public_definitions(self):
        good = ('namespace pr_impl {\n\nnamespace {\n\nvoid local() {}\n\n} // namespace\n\n'
                'void internal() {}\n\n} // namespace pr_impl\n\nusing namespace pr_impl;\n\n'
                'void prPublic() {}\n')
        self.assertEqual(self.rules(lint.check_namespace_layout, 'lib/core/src/a.cpp', good, 'pr_impl'), [])
        internal = 'namespace pr_impl {\n\nvoid internal() {}\n\n} // namespace pr_impl\n'
        self.assertEqual(self.rules(lint.check_namespace_layout, 'lib/core/src/b.cpp', internal, 'pr_impl'), [])
        bad = ('using namespace pr_impl;\n\nnamespace {\n\nvoid local() {}\n\n} // namespace\n\n'
               'namespace pr_impl {\n\n} // namespace pr_impl\n\nvoid prPublic() {}\n')
        self.assertEqual(sorted(self.rules(lint.check_namespace_layout, 'lib/core/src/c.cpp', bad, 'pr_impl')),
                         [(1, 'namespace-layout'), (3, 'namespace-layout'), (11, 'namespace-layout')])

    def test_a_source_of_the_cpp_api_is_one_pr_block(self):
        good = ('using namespace pr_impl;\n\nnamespace pr {\n\nnamespace {\n\nvoid local() {}\n\n'
                '} // namespace\n\nvoid jsRun() {}\n\n} // namespace pr\n')
        self.assertEqual(self.rules(lint.check_namespace_layout, 'lib/core/quickjs_cpp/src/a.cpp', good, 'pr'), [])
        self.assertEqual(self.rules(lint.check_namespace, 'lib/core/quickjs_cpp/src/a.cpp', good, False, 'pr'), [])
        bad = 'namespace pr_impl {\n\nvoid jsRun() {}\n\n} // namespace pr_impl\n'
        self.assertEqual(self.rules(lint.check_namespace, 'lib/core/quickjs_cpp/src/b.cpp', bad, False, 'pr'),
                         [(1, 'namespace'), (1, 'namespace')])
        self.assertEqual(self.rules(lint.check_namespace, 'lib/core/src/c.cpp', good, False, 'pr_impl'),
                         [(1, 'namespace'), (3, 'namespace')])

    def test_public_definitions_stay_out_of_pr_impl(self):
        source = ('namespace pr_impl {\n\nvoid internal() {}\n\nvoid prPublic() {}\n\n'
                  '} // namespace pr_impl\n')
        self.assertEqual(self.rules(lint.check_namespace, 'lib/core/src/a.cpp', source, True, 'pr_impl'),
                         [(5, 'namespace')])
        self.assertEqual(self.rules(lint.check_namespace, 'lib/core/src/b.cpp', 'void run() {}\n',
                                    False, 'pr_impl'), [(1, 'namespace')])

    def test_the_prefixes_are_the_public_api(self):
        names = lint.public_names(['typedef struct PrThing PrThing;\nvoid prThingRun(PrThing* t);\n'
                                   'PR_HANDLE_UPCAST(Thing, Base, Base)\n'])
        source = ('@interface PrView\n@end\n'
                  'void run(PrThing* t) { prThingRun(t); prThingAsBase(t); prHelper(); }\n')
        self.assertEqual(self.rules(lint.check_internal_names, 'lib/core/src/a.mm', source, names),
                         [(3, 'internal-name')])

    def test_a_public_type_name_is_its_handle_base_alone(self):
        header = ('typedef struct PrThing PrThing;\ntypedef uint8_t PrKind;\n'
                  'typedef struct PrPoint {\n    float x;\n} PrPoint;\ntypedef void (*PrVisitFn)(void* c);\n')
        types = lint.public_types([header])
        self.assertEqual(types, {'Thing', 'Kind', 'Point', 'VisitFn'})
        bases = lint.handle_bases(['struct PrThing : pr_impl::Thing {};\n'])
        source = ('namespace pr_impl {\n\nstruct Thing {\n};\n\ntypedef uint8_t Kind;\n\nstruct Point;\n\n'
                  'using VisitFn = int;\n\nstruct Other {\n};\n\n} // namespace pr_impl\n')
        self.assertEqual(self.rules(lint.check_shadowed_types, 'lib/core/src/a.cpp', source, types, bases),
                         [(6, 'shadowed-type'), (8, 'shadowed-type'), (10, 'shadowed-type')])

    def test_an_internal_enum_kept_in_step_with_a_public_one_is_reported(self):
        source = ('static_assert(int(PrThingKind_A) == int(ThingKind_A), "aligned");\n'
                  'static_assert(uint32_t(PrDrawExtend_Pad) == PrFontExtend_Pad);\n'
                  'static_assert(countOf(kNames) == PrThingKind_Count);\n'
                  'static_assert(int(Base_Length) == int(PrCssBase_Length)\n'
                  '              && true, "aligned");\n')
        self.assertEqual(self.rules(lint.check_parallel_enums, 'lib/core/src/a.cpp', source),
                         [(1, 'parallel-enum'), (4, 'parallel-enum')])

    def test_a_public_constant_is_no_macro_unless_the_preprocessor_reads_it(self):
        header = ('#define PR_THING_MAX 64\n#define PR_THING_NATIVE 1\n#if PR_THING_NATIVE\n#endif\n'
                  '#define PR_THING(x) (x)\n')
        self.assertEqual(self.rules(lint.check_macro_constants, 'lib/core/include/pr/a.h', header),
                         [(1, 'macro-constant')])

    def test_an_array_names_its_index_domain_by_a_type_or_a_known_word(self):
        header = ('struct A {\n'
                  '    Array<int> a;   // by LayoutNodeIndex\n'
                  '    Array<int> b;   // by byte: its advance\n'
                  '    Array<int> c;   // by Missing\n'
                  '    Array<int> d;   // by thing\n'
                  '    Array<int> e;   // sorted by thing\n'
                  '};\n')
        self.assertEqual(self.rules(lint.check_index_domains, 'lib/core/src/a.h', header, {'LayoutNodeIndex'}),
                         [(4, 'index-domain'), (5, 'index-domain')])

    def test_c_linkage_declares_only_what_c_defines(self):
        source = ('extern "C" int prTestCApi(void);\nextern "C" {\n}\nextern "C" int prDefined() {\n'
                  '    return 0;\n}\n')
        self.assertEqual(self.rules(lint.check_linkage, 'lib/core/tests/test_a.cpp', source),
                         [(2, 'linkage'), (4, 'linkage')])


class BindingIncludes(unittest.TestCase):
    def test_bindings_reach_modules_through_public_headers(self):
        report = lint.Report()
        lint.check_binding_includes(report, 'lib/css/quickjs/src/css_quickjs.cpp',
                                    '#include "pr/css.h"\n#include "pr/quickjs_binding.h"\n'
                                    '#include "pr/webidl_quickjs.h"\n#include "pr/array.h"\n'
                                    '#include "css/quickjs/src/cssom_rules.h"\n'
                                    '#include "css/src/css_property.h"\n')
        self.assertEqual([rule for _, _, rule, _ in report.problems], ['binding-include'])


class PrivateIncludes(unittest.TestCase):
    def problems(self, relative, text):
        report = lint.Report()
        lint.check_private_includes(report, os.path.join(lint.ROOT, relative), text)
        return [rule for _, _, rule, _ in report.problems]

    def test_a_library_reaches_its_own_implementation(self):
        self.assertEqual(self.problems('lib/path/tests/test_path.cpp', '#include "path/src/flatten.h"\n'), [])
        self.assertEqual(self.problems('lib/dom/quickjs/src/dom_quickjs.cpp',
                                       '#include "dom/quickjs/src/dom_events.h"\n'), [])

    def test_a_library_does_not_reach_another_implementation(self):
        self.assertEqual(self.problems('lib/svg/src/svg_parse.cpp', '#include "path/src/flatten.h"\n'),
                         ['private-include'])

    def test_only_a_module_reaches_its_tests(self):
        self.assertEqual(self.problems('lib/html/tests/test_html_page.cpp',
                                       '#include "html/tests/support/html_test_fetch.h"\n'), [])
        self.assertEqual(self.problems('lib/html/quickjs/tests/test_html_quickjs.cpp',
                                       '#include "html/tests/support/html_test_fetch.h"\n'), [])
        self.assertEqual(self.problems('lib/html/tests/test_html_page.cpp', '#include "layout/tests/layout_fixture.h"\n'),
                         ['private-include'])
        self.assertEqual(self.problems('tools/shell/page_host.cpp', '#include "net/tests/net_peer.h"\n'), ['private-include'])
        self.assertEqual(self.problems('tools/shell/page_host.cpp', '#include "url/src/url_record.h"\n'),
                         ['private-include'])

    def test_the_web_libraries_share_their_implementations(self):
        self.assertEqual(self.problems('lib/html/src/html.cpp', '#include "dom/src/dom_node.h"\n'), [])


class ModuleDependencies(unittest.TestCase):
    CMAKE = ('add_library(core STATIC lib/core/src/memory.cpp)\n'
             'target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib/core/include)\n'
             'add_library(geom STATIC lib/geom/src/geom.cpp)\n'
             'target_include_directories(geom PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib/geom/include)\n'
             'target_link_libraries(geom PUBLIC core)\n'
             'add_library(path STATIC lib/path/src/path.cpp)\n'
             'target_include_directories(path PUBLIC\n    ${CMAKE_CURRENT_SOURCE_DIR}/lib/path/include\n)\n'
             'target_link_libraries(path PUBLIC geom)\n')

    def problems(self, cmake, files):
        files = dict(files)
        files.setdefault('lib/core/include/pr/memory.h', '')
        files.setdefault('lib/core/src/memory.cpp', '')
        files.setdefault('lib/geom/include/pr/geom.h', '')
        files.setdefault('lib/geom/src/geom.cpp', '')
        files.setdefault('lib/geom/src/hull.h', '')
        files.setdefault('lib/path/include/pr/path.h', '')
        files.setdefault('lib/path/src/path.cpp', '')
        with tempfile.TemporaryDirectory() as root:
            for path, text in {'CMakeLists.txt': cmake, **files}.items():
                Path(root, path).parent.mkdir(parents=True, exist_ok=True)
                Path(root, path).write_text(text)
            report = lint.Report()
            with patch.object(lint, 'ROOT', root):
                lint.check_module_dependencies(report)
            return [(path, message) for path, _, _, message in report.problems]

    def test_a_target_links_each_module_it_includes(self):
        self.assertEqual(self.problems(self.CMAKE, {'lib/path/src/path.cpp': '#include "pr/geom.h"\n'}), [])

    def test_a_module_reached_through_another_is_not_linked(self):
        self.assertEqual(self.problems(self.CMAKE.replace('target_link_libraries(path PUBLIC geom)\n',
                                                          'target_link_libraries(path PUBLIC core)\n'),
                                       {'lib/path/src/path.cpp': '#include "pr/geom.h"\n'}),
                         [('lib/path/src/path.cpp', 'path includes pr/geom.h, of geom, which it does not link')])

    def test_core_is_linked_by_every_target(self):
        cmake = self.CMAKE.replace('target_link_libraries(path PUBLIC geom)\n', '')
        self.assertEqual(self.problems(cmake, {'lib/path/src/path.cpp': '#include "pr/memory.h"\n'}), [])

    def test_a_public_header_includes_what_is_linked_publicly(self):
        cmake = self.CMAKE.replace('target_link_libraries(path PUBLIC geom)', 'target_link_libraries(path PRIVATE geom)')
        self.assertEqual(self.problems(cmake, {'lib/path/include/pr/path.h': '#include "pr/geom.h"\n'}),
                         [('lib/path/include/pr/path.h',
                           'a public header of path includes pr/geom.h, of geom, which it links privately')])

    def test_a_link_that_nothing_includes_is_reported(self):
        self.assertEqual(self.problems(self.CMAKE, {}),
                         [('CMakeLists.txt', 'path links geom, but includes none of its headers')])

    def test_an_internal_include_uses_its_module(self):
        cmake = self.CMAKE + 'add_executable(hull_tests lib/geom/tests/test_hull.cpp)\n' \
                             'target_link_libraries(hull_tests PRIVATE geom)\n'
        files = {'lib/path/src/path.cpp': '#include "pr/geom.h"\n',
                 'lib/geom/tests/test_hull.cpp': '#include "geom/src/hull.h"\n'}
        self.assertEqual(self.problems(cmake, files), [])

    def test_an_internal_include_uses_what_the_internal_header_includes(self):
        cmake = self.CMAKE + 'add_executable(path_tests lib/path/tests/test_flatten.cpp)\n' \
                             'target_link_libraries(path_tests PRIVATE path geom)\n'
        files = {'lib/path/src/path.cpp': '#include "pr/geom.h"\n',
                 'lib/path/src/flatten.h': '#include "pr/geom.h"\n',
                 'lib/path/tests/test_flatten.cpp': '#include "path/src/flatten.h"\n'}
        self.assertEqual(self.problems(cmake, files), [])

    def test_a_target_named_as_a_module_is_apart_from_its_library(self):
        cmake = ('pr_library(geom INCLUDE lib/geom/include lib/geom/src/geom.cpp)\n'
                 'pr_example(geom examples/geom/main.cpp)\n'
                 'target_link_libraries(geom PRIVATE prax::geom)\n')
        self.assertEqual(self.problems(cmake, {'examples/geom/main.cpp': '#include "pr/geom.h"\n'}), [])

    def test_a_link_to_an_engine_target_that_is_not_declared_is_reported(self):
        cmake = ('pr_library(geom INCLUDE lib/geom/include lib/geom/src/geom.cpp)\n'
                 'if (WIN32)\n    target_link_libraries(pr_geom PRIVATE prax::core_windows)\nendif()\n')
        self.assertEqual(self.problems(cmake, {}),
                         [('CMakeLists.txt', 'geom links prax::core_windows, which no target is')])

    def test_an_alias_is_a_target(self):
        cmake = ('add_library(pr_zlib STATIC lib/zlib/zlib.c)\n'
                 'add_library(prax::zlib ALIAS pr_zlib)\n'
                 'pr_library(geom INCLUDE lib/geom/include lib/geom/src/geom.cpp)\n'
                 'target_link_libraries(pr_geom PRIVATE prax::zlib)\n')
        self.assertEqual(self.problems(cmake, {}), [])

    def test_a_tree_without_a_build_has_no_targets(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertEqual(lint.modules.targets(root), {})


class BackendIncludes(unittest.TestCase):
    def test_backends_reach_modules_through_public_headers(self):
        report = lint.Report()
        path = os.path.join(lint.ROOT, 'lib/draw/backends/cpu/src/draw_cpu.cpp')
        lint.check_backend_includes(report, path,
                                    '#include "pr/draw_renderer.h"\n#include "pr/array.h"\n'
                                    '#include "draw/backends/cpu/src/span.h"\n'
                                    '#include "draw/src/list_data.h"\n'
                                    '#include "path/src/flatten.h"\n')
        self.assertEqual([rule for _, _, rule, _ in report.problems],
                         ['backend-include', 'backend-include'])

    def test_backend_tests_reach_their_interfaces_tests(self):
        report = lint.Report()
        path = os.path.join(lint.ROOT, 'lib/gpu/backends/metal/tests/test_gpu_metal.mm')
        lint.check_backend_includes(report, path,
                                    '#include "gpu/tests/kTestGpuNativeShaders.h"\n'
                                    '#include "draw/tests/draw_testing.h"\n')
        self.assertEqual([rule for _, _, rule, _ in report.problems], ['backend-include'])
        report = lint.Report()
        path = os.path.join(lint.ROOT, 'lib/gpu/backends/metal/src/gpu_metal.mm')
        lint.check_backend_includes(report, path, '#include "gpu/tests/kTestGpuNativeShaders.h"\n')
        self.assertEqual([rule for _, _, rule, _ in report.problems], ['backend-include'])

    def test_only_nested_modules_are_backends(self):
        self.assertEqual(lint.backend_of(os.path.join(lint.ROOT, 'lib/draw/backends/cpu/src/draw_cpu.cpp')),
                         'lib/draw/backends/cpu')
        self.assertIsNone(lint.backend_of(os.path.join(lint.ROOT, 'lib/draw/src/draw_list.cpp')))
        self.assertIsNone(lint.backend_of(os.path.join(lint.ROOT, 'lib/dom/src/dom.cpp')))

    def test_comparisons_follow_the_rules_of_backends(self):
        path = os.path.join(lint.ROOT, 'lib/draw/comparisons/blend2d/src/draw_blend2d.cpp')
        self.assertEqual(lint.backend_of(path), 'lib/draw/comparisons/blend2d')
        self.assertEqual(lint.backend_file_prefix(lint.backend_of(path)), 'draw_blend2d')
        report = lint.Report()
        lint.check_backend_includes(report, path, '#include "pr/draw.h"\n'
                                                  '#include "draw/comparisons/blend2d/src/x.h"\n'
                                                  '#include "draw/src/list_data.h"\n')
        self.assertEqual([rule for _, _, rule, _ in report.problems], ['backend-include'])
        self.assertNotIn(lint.backend_of(path).split('/')[-1], lint.OWN_BACKENDS)


class SpecFree(unittest.TestCase):
    def test_spec_free_libraries_name_no_spec(self):
        report = lint.Report()
        ids = {'css-backgrounds-3': {'corner-overlap'}}
        lint.check_spec_free(report, os.path.join(lint.ROOT, 'lib/geom/src/geom.cpp'),
                             '// css-backgrounds-3 - https://drafts.csswg.org/css-backgrounds/\n'
                             '// as css-backgrounds-3/#corner-overlap scales them\n'
                             '// font-rs - https://github.com/raphlinus/font-rs\n', ids)
        self.assertEqual(sorted(line for _, line, _, _ in report.problems), [1, 2])


class LineLength(unittest.TestCase):
    def problems(self, source):
        report = lint.Report()
        lint.check_line_length(report, os.path.join(lint.ROOT, 'lib/core/src/example.cpp'), source,
                               lint.c_literals(source))
        return [line for _, line, _, _ in report.problems]

    def python_problems(self, source):
        report = lint.Report()
        lint.check_line_length(report, os.path.join(lint.ROOT, 'tools/example.py'), source,
                               lint.python_literals(source))
        return [line for _, line, _, _ in report.problems]

    def test_code_and_prose_end_by_the_limit(self):
        code = 'int value = ' + ' + '.join(['term'] * 30) + ';\n'
        prose = '// ' + ' '.join(['word'] * 30) + '\n'
        self.assertEqual(self.problems('ok;\n' + code + prose), [2, 3])

    def test_what_cannot_break_may_run_past(self):
        literal = 'TEST_CASE("' + 'a' * 110 + '") {\n'
        reference = ' ' * 60 + '// (cssom-1/#' + 'x' * 60 + ')\n'
        raw = 'R"JS(\n' + 'x' * 120 + '\n)JS";\n'
        self.assertEqual(self.problems(literal + reference + raw), [])

    def test_code_after_a_literal_breaks(self):
        self.assertEqual(self.problems('f("' + 'a' * 110 + '", value, other, more);\n'), [1])

    def test_python_is_held_to_the_limit_too(self):
        code = 'value = [' + ', '.join(['term'] * 25) + ']\n'
        prose = '# ' + ' '.join(['word'] * 30) + '\n'
        literal = 'TABLE = {1: "' + 'a' * 100 + '"}\n'
        reference = ' ' * 60 + '# (cssom-1/#' + 'x' * 60 + ')\n'
        formatted = 'text = f"{value}' + 'b' * 100 + '"\n'
        self.assertEqual(self.python_problems(code + prose + literal + reference + formatted),
                         [1, 2])


class Whitespace(unittest.TestCase):
    def problems(self, data, path='lib/core/src/example.cpp'):
        report = lint.Report()
        lint.check_whitespace(report, os.path.join(lint.ROOT, path), data)
        return [(line, message) for _, line, _, message in report.problems]

    def test_a_line_ends_with_lf_alone(self):
        self.assertEqual(self.problems(b'ok;\r\nok;\n'), [(1, 'line ends with CRLF, not LF')])

    def test_a_line_ends_without_whitespace(self):
        self.assertEqual(self.problems(b'ok; \n\t\nok;\n'),
                         [(1, 'line ends with whitespace'), (2, 'line ends with whitespace')])

    def test_the_last_line_ends(self):
        self.assertEqual(self.problems(b'ok;\nok;'), [(2, 'file does not end with a newline')])
        self.assertEqual(self.problems(b''), [])

    def test_a_patch_keeps_its_blank_context_lines(self):
        self.assertEqual(self.problems(b'@@ -1,3 +1,3 @@\n \n-a\n+b\n', 'patches/a.patch'), [])
        self.assertEqual(self.problems(b' \r\n', 'patches/a.patch'), [(1, 'line ends with CRLF, not LF')])

    def test_binary_is_not_text(self):
        self.assertEqual(self.problems(b'\x89PNG\r\n\x1a\n\0\0 '), [])


class Allocation(unittest.TestCase):
    def problems(self, source, path='lib/html/src/example.cpp'):
        report = lint.Report()
        lint.check_allocation(report, os.path.join(lint.ROOT, path), source)
        return report.problems

    def test_objective_c_new_is_a_message(self):
        self.assertEqual(self.problems('id d = [[Descriptor new] autorelease];\n', 'lib/gpu/src/example.mm'), [])
        self.assertEqual(len(self.problems('Object* o = new Object;\n', 'lib/gpu/src/example.mm')), 1)

    def test_allocation_without_headers(self):
        for expression in ('new Object{}', '::new Object[4]', 'new (storage) Object',
                           '::operator new(8)', 'delete object', 'delete[] objects',
                           '::operator delete(object)'):
            with self.subTest(expression=expression):
                problems = self.problems('// Generated by a fixture\n' + expression + ';\n')
                self.assertEqual(len(problems), 1)
                self.assertEqual(problems[0][1], 2)

    def test_new_header(self):
        for include in ('#include <new>', '# include "new"', '#include<new>'):
            with self.subTest(include=include):
                self.assertEqual(len(self.problems(include)), 1)

    def test_comments_literals_and_deleted_functions(self):
        self.assertEqual(self.problems('''
            // new Object; delete object;
            /* #include <new> */
            const char* text = "new Object; delete object;";
            const char* raw = R"tag("new" and delete /* anything */
#include <new>
)tag";
            Object(const Object&) = /* deliberately */ delete;
            void* renewed = alloc<Object>();
            prMemoryFree(renewed);
        '''), [])


class CommentPlacement(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_comment_placement(report, os.path.join(lint.ROOT, 'lib/draw/include/pr/example.h'), text)
        return [line for _, line, _, _ in report.problems]

    def test_a_comment_directly_after_a_declaration(self):
        self.assertEqual(self.problems('void a(void);\n// About b.\nvoid b(void);\n'), [2])
        self.assertEqual(self.problems('struct S {\n    int a;  // trailing\n    // About b.\n    int b;\n};\n'), [3])

    def test_a_comment_set_apart(self):
        self.assertEqual(self.problems('void a(void);\n\n// About b.\nvoid b(void);\n'), [])

    def test_a_statement_in_a_function_body(self):
        self.assertEqual(self.problems('inline int f(int x) {\n    x += 1;\n    // Then double it.\n    return 2 * x;\n}\n'),
                         [])

    def test_a_comment_in_a_literal(self):
        self.assertEqual(self.problems('const char* s = R"(\nvar a = 1;\n// not a comment\n)";\n'), [])


class ParameterNames(unittest.TestCase):
    def problems(self, header, source):
        report = lint.Report()
        lint.check_parameter_names(report, 'x.h', header, 'x.cpp', source)
        return [message for _, _, _, message in report.problems]

    def test_a_definition_names_its_parameters_as_its_declaration(self):
        self.assertEqual(self.problems('void f(int count, Thing* thing);\n', 'void f(int count, Thing* thing) {\n}\n'),
                         [])
        self.assertEqual(self.problems('void f(int count, Thing* thing);\n', 'void f(int n, Thing* thing) {\n}\n'),
                         ['parameter 1 of f is `n`, and `count` in x.h'])

    def test_unnamed_and_overloaded_parameters_are_not_compared(self):
        self.assertEqual(self.problems('void f(const Thing* thing);\n', 'void f(const Thing*) {\n}\n'), [])
        self.assertEqual(self.problems('void f(int a);\nvoid f(float b);\n',
                                       'void f(int x) {\n}\nvoid f(float y) {\n}\n'), [])

    def test_a_function_pointer_and_a_default_value(self):
        self.assertEqual(self.problems('void f(void (*fn)(int), int count = 0);\n',
                                       'void f(void (*callback)(int), int count) {\n}\n'),
                         ['parameter 1 of f is `callback`, and `fn` in x.h'])


class CommentWrap(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_comment_wrap(report, 'x.h', text)
        return [line for _, line, _, _ in report.problems]

    def test_a_line_breaks_where_it_reaches_the_limit(self):
        self.assertEqual(self.problems('// ' + 'word ' * 10 + '\n// next\n'), [2])
        self.assertEqual(self.problems('// ' + ' '.join(['word'] * 23) + '\n// next\n'), [])

    def test_the_report_names_rewrap_with_the_files_it_fixes(self):
        report = lint.Report()
        lint.check_comment_wrap(report, os.path.join(lint.ROOT, 'x.h'), '// ' + 'word ' * 10 + '\n// next\n')
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            report.print()
        self.assertIn('fix comment-wrap with: ./dev.sh rewrap x.h', output.getvalue())

    def test_lines_that_stand_on_their_own(self):
        self.assertEqual(self.problems('// Short.\n//\n// Next paragraph.\n'), [])
        self.assertEqual(self.problems('// A list:\n// - one\n// - two\n'), [])
        self.assertEqual(self.problems('// Example:\n//   code(x);\n'), [])
        self.assertEqual(self.problems('//   External  bytes\n//   Inline    bytes\n'), [])
        self.assertEqual(self.problems('// Title\n//\n// a-1 - https://a.example/\n// b-1 - https://b.example/\n'),
                         [])


class LeaningComments(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_leaning_comments(report, 'x.h', text)
        return [line for _, line, _, _ in report.problems]

    def test_a_possessive_whose_noun_the_name_supplies(self):
        self.assertEqual(self.problems("float width;  // a stroke's\n"), [1])
        self.assertEqual(self.problems("int claims;  // the last round's, by node\n"), [1])
        self.assertEqual(self.problems("int precision;  // as PrKitNumber's\n"), [1])
        self.assertEqual(self.problems("void* context;  // the callbacks'\n"), [1])

    def test_a_phrase_that_names_what_it_holds(self):
        self.assertEqual(self.problems("float width;  // the stroke's width, in px\n"), [])
        self.assertEqual(self.problems("int claims;  // the keys the round's build claimed\n"), [])
        self.assertEqual(self.problems("int a;  // its margin collapses with its first child's\n"), [])
        self.assertEqual(self.problems("// A stroke's width.\nfloat width;\n"), [])


class TrailingComments(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_trailing_comments(report, 'x.h', text)
        return [line for _, line, _, _ in report.problems]

    def test_a_comment_beside_code_continues_aligned_with_it(self):
        self.assertEqual(self.problems('int a;  // one\n        // two\n'), [])
        self.assertEqual(self.problems('int a;  // one\n        // two\nint b;  // three\n'), [])
        self.assertEqual(self.problems('int a;  // one\nint b;  // two\n'), [])
        self.assertEqual(self.problems('int a;  // one\n\n// About b.\nint b;\n'), [])

    def test_a_comment_below_one_beside_code_at_another_column(self):
        self.assertEqual(self.problems('int a;  // one\n// About b.\nint b;\n'), [2])
        self.assertEqual(self.problems('int a;  // one\n        // two\n// About b.\nint b;\n'), [3])


class Documented(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_documented(report, 'x.h', text)
        return [line for _, line, _, _ in report.problems]

    def test_a_declaration_is_under_a_comment(self):
        self.assertEqual(self.problems('void a(void);\n'), [1])
        self.assertEqual(self.problems('// About a and b.\nvoid a(void);\nvoid b(void);\n'), [])
        self.assertEqual(self.problems('// About a.\nvoid a(void);\n\nvoid b(void);\n'), [4])

    def test_a_comment_covers_a_definition_with_blank_lines_inside(self):
        self.assertEqual(self.problems('// About S.\nstruct S {\n    int a;\n\n    int b;\n};\n'), [])

    def test_what_declares_nothing(self):
        self.assertEqual(self.problems('#pragma once\n\n#include "pr/base.h"\n\nPR_ASSUME_NONNULL_BEGIN\n\n'
                                       'namespace pr_impl {\n\nstruct S;\nusing pr_impl::T;\n\n'
                                       '} // namespace pr_impl\n\nPR_ASSUME_NONNULL_END\n'), [])
        self.assertEqual(self.problems('#if defined(__cplusplus)\nextern "C" {\n#endif\n\n'
                                       '#if defined(__cplusplus)\n}\n#endif\n'), [])

    def test_a_blank_line_in_a_literal_does_not_end_a_run(self):
        script = '// A script.\nconstexpr const char* kScript = R"JS(\na();\n\nb();\n)JS";\n'
        self.assertEqual(self.problems(script), [])

    def test_a_section_title_is_not_documentation(self):
        rule = '// ' + '-' * 117 + '\n'
        self.assertEqual(self.problems(rule + '// Name\n\nvoid a(void);\n'), [4])
        self.assertEqual(self.problems('// --- Name ---\nvoid a(void);\n'), [2])
        self.assertEqual(self.problems('// --- Name ---\n// About a.\nvoid a(void);\n'), [])


class Sections(unittest.TestCase):
    RULE = '// ' + '-' * 117

    def problems(self, text):
        report = lint.Report()
        lint.check_sections(report, os.path.join(lint.ROOT, 'lib/draw/include/pr/example.h'), text)
        return [line for _, line, _, _ in report.problems]

    def test_well_formed(self):
        self.assertEqual(self.problems(self.RULE + '\n// Shapes\n\n// --- Strokes ---\n'), [])

    def test_a_short_rule(self):
        self.assertEqual(self.problems('// ------------------------------\n// Shapes\n'), [1])

    def test_a_rule_without_a_name(self):
        self.assertEqual(self.problems(self.RULE + '\n\nvoid a(void);\n'), [1])

    def test_a_rule_below_the_name(self):
        self.assertEqual(self.problems(self.RULE + '\n// Shapes\n' + self.RULE + '\n'), [3])

    def test_a_malformed_subsection(self):
        self.assertEqual(self.problems('// -- Strokes --\n'), [1])


class DocLinks(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_doc_links(report, os.path.join(lint.ROOT, 'lib/draw/src/example.cpp'), text)
        return report.problems

    def test_a_pointer_into_docs(self):
        self.assertEqual(len(self.problems('// The list is frozen (docs/conventions.md#ownership).\n')), 1)
        self.assertEqual(len(self.problems('// Its fidelity (draw.md#backends).\n')), 1)

    def test_other_names(self):
        self.assertEqual(self.problems('// A spec (https://example.org/draw.mdx) and notes.md.\n'), [])


class ImplementationNames(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_implementation_names(report, os.path.join(lint.ROOT, 'lib/layout/src/example.cpp'), text)
        return report.problems

    def test_an_implementation_in_a_comment_or_string(self):
        self.assertEqual(len(self.problems('// Rounded to a unit, as Chrome rounds it.\n')), 1)
        self.assertEqual(len(self.problems('/* Safari and Firefox agree. */\n')), 2)
        self.assertEqual(len(self.problems('TEST_CASE("clamps after layout, as Blink does") {}\n')), 1)
        self.assertEqual(len(self.problems('// As lottie-web draws it.\n')), 1)
        self.assertEqual(len(self.problems('// Yoga and Taffy place it at the start.\n')), 2)
        self.assertEqual(len(self.problems('// Joined as Skia and Cairo join them.\n')), 2)

    def test_implementations_as_a_group(self):
        self.assertEqual(len(self.problems('// A whole turn is the identity, as browsers make it.\n')), 1)
        self.assertEqual(len(self.problems('// All three browsers compute these alike.\n')), 1)
        self.assertEqual(len(self.problems('// A hue accepts any keyword, as engines do.\n')), 1)
        self.assertEqual(self.problems('// Creates a browser. Its browsers must be destroyed first.\n'), [])
        self.assertEqual(len(self.problems('// A dashed ident, which every engine writes as is.\n')), 1)
        self.assertEqual(len(self.problems('// Loops from its in point, the way web players loop it.\n')), 1)
        self.assertEqual(self.problems('// Every engine object is allocated as its handle.\n'), [])
        self.assertEqual(self.problems("// The start of every browser's state.\n"), [])

    def test_libraries(self):
        self.assertEqual(len(self.problems('// The triangle filter of libjpeg.\n')), 1)
        self.assertEqual(len(self.problems('// The accumulation scheme of font-rs.\n')), 1)

    def test_own_backends_are_checked(self):
        path = os.path.join(lint.ROOT, 'lib/image/backends/builtin/src/example.cpp')
        backend = lint.backend_of(path)
        self.assertIn(backend.split('/')[-1], lint.OWN_BACKENDS)
        self.assertNotIn(lint.backend_of(os.path.join(lint.ROOT, 'lib/text/backends/harfbuzz/src/x.cpp'))
                         .split('/')[-1], lint.OWN_BACKENDS)

    def test_vendor_prefixes_and_code(self):
        self.assertEqual(self.problems('// The -webkit-box-decoration-break alias.\n'), [])
        self.assertEqual(self.problems('CHECK(parse("::-WebKit-Thing"));\n'), [])
        self.assertEqual(self.problems('int Chrome = 0;\n'), [])


class DanglingNames(unittest.TestCase):
    def problems(self, text, path='lib/layout/src/example.cpp'):
        report = lint.Report()
        defined = {'prLayoutTreeLayout', 'prLayoutBoxStyleIntern', 'layoutBlock', 'kLayoutFaceNone', 'textPath'}
        lint.check_dangling_names(report, os.path.join(lint.ROOT, path), text, defined)
        return report.problems

    def test_a_public_name(self):
        self.assertEqual(self.problems('// Lays out first (prLayoutTreeLayout).\n'), [])
        self.assertEqual(len(self.problems('// Lays out first, as prLayoutTreeRelayout does.\n')), 1)

    def test_an_internal_name_cited_as_a_name(self):
        self.assertEqual(self.problems('// The block (layoutBlock), or kLayoutFaceNone.\n'), [])
        self.assertEqual(len(self.problems('// The block (layoutBlocks).\n')), 1)
        self.assertEqual(len(self.problems('// Calls layoutFlow() first.\n')), 1)
        self.assertEqual(self.problems('// A kHz unit, a textArea element.\n'), [])

    def test_a_document(self):
        self.assertEqual(self.problems('Each group interns by `prLayoutBoxStyle*`.\n', 'docs/layout.md'), [])
        self.assertEqual(len(self.problems('Lays out by `prLayoutTreeReflow`.\n', 'docs/layout.md')), 1)
        self.assertEqual(self.problems('A `PrSketch` is illustrative.\n', 'docs/conventions.md'), [])


class CountOf(unittest.TestCase):
    def problems(self, source):
        report = lint.Report()
        lint.check_count_of(report, os.path.join(lint.ROOT, 'lib/core/src/example.cpp'), source)
        return report.problems

    def test_hand_rolled_counts(self):
        for expression in ('sizeof(kTable) / sizeof(kTable[0])', 'sizeof kTable / sizeof *kTable',
                           'sizeof(rows)/sizeof(*rows)', 'std::size(kHosts)'):
            with self.subTest(expression=expression):
                self.assertEqual(len(self.problems('auto n = %s;\n' % expression)), 1)

    def test_other_sizes(self):
        for expression in ('sizeof(a) / sizeof(b)', 'sizeof(Entry) * count', 'countOf(kTable)',
                           '// sizeof(kTable) / sizeof(kTable[0])'):
            with self.subTest(expression=expression):
                self.assertEqual(self.problems('auto n = %s;\n' % expression), [])


class Enums(unittest.TestCase):
    def test_named_enums_are_rejected(self):
        for declaration in ('enum Value {', 'enum class Value {', 'enum Value : uint8_t {'):
            with self.subTest(declaration=declaration):
                report = lint.Report()
                lint.check_enums(report, 'enum.h', declaration + '\n    Value_A,\n};\n')
                self.assertIn('enum-form', [rule for _, _, rule, _ in report.problems])

    def test_named_enums_in_a_class_are_rejected(self):
        report = lint.Report()
        lint.check_enums(report, 'enum.h', 'class Host {\n    enum class Value {\n        A,\n    };\n};\n')
        self.assertIn('enum-form', [rule for _, _, rule, _ in report.problems])

    def test_fixed_width_enum_is_accepted(self):
        report = lint.Report()
        lint.check_enums(report, 'enum.h',
                         'enum {\n    Value_A,\n    Value_B,\n};\ntypedef uint8_t Value;\n')
        self.assertEqual(report.problems, [])

    def test_members_are_named_for_their_type(self):
        report = lint.Report()
        lint.check_enums(report, 'enum.h',
                         'enum {\n    Value_A,\n    Other_B,\n};\ntypedef uint8_t ValueKind;\n')
        self.assertEqual([rule for _, _, rule, _ in report.problems],
                         ['enum-member-name', 'enum-member-name'])

    def test_flags_combine_with_the_values_of_their_type(self):
        report = lint.Report()
        lint.check_enums(report, 'enum.h',
                         'enum {\n    Value_A,\n    ValueFlag_B = 1u << 6,\n};\ntypedef uint8_t Value;\n')
        self.assertEqual(report.problems, [])

    def test_a_set_of_flags_names_each_flag(self):
        report = lint.Report()
        lint.check_enums(report, 'enum.h',
                         'enum {\n    ValueFlag_A = 1,\n    ValueFlag_B = 2,\n};\n'
                         'typedef uint8_t ValueFlags;\n')
        self.assertEqual(report.problems, [])


    def test_a_cast_of_a_member_to_its_type_is_rejected_wherever_the_type_is(self):
        report = lint.Report()
        lint.check_enums(report, 'use.h', 'v = Value(Value_A);\nreturn Value(Value_B);\n', {'Value'})
        self.assertEqual([rule for _, _, rule, _ in report.problems], ['enum-cast', 'enum-cast'])

    def test_a_cast_that_gives_a_conditional_its_type_is_accepted(self):
        report = lint.Report()
        lint.check_enums(report, 'use.h', 'v = c ? Value(Value_A) : other;\nw = c ? other : Value(Value_B);\n'
                         'x = c ? Value(Value_A)\n      : other;\n', {'Value'})
        self.assertEqual(report.problems, [])


class IncludeOrder(unittest.TestCase):
    def problems(self, relative, text):
        report = lint.Report()
        lint.check_include_order(report, os.path.join(lint.ROOT, relative), text)
        return [message for _, _, _, message in report.problems]

    def test_sections_are_in_order_and_sorted(self):
        self.assertEqual(self.problems('lib/css/src/css_rule.cpp',
                                       '#include "css/src/css_rule.h"\n\n'
                                       '#include "pr/string.h"\n\n'
                                       '#include "css/src/css_grammar.h"\n'
                                       '#include "css/src/css_value.h"\n'), [])

    def test_a_section_needs_its_blank_line(self):
        self.assertEqual(len(self.problems('lib/dom/src/x.h',
                                           '#include "pr/string.h"\n'
                                           '#include "css/src/css_value.h"\n')), 1)

    def test_a_section_sorts(self):
        self.assertEqual(len(self.problems('lib/dom/src/x.h',
                                           '#include "core/src/b.h"\n'
                                           '#include "core/src/a.h"\n')), 1)

    def test_third_party_headers_are_quoted_and_confined(self):
        self.assertEqual(len(self.problems('lib/core/tests/test_x.cpp',
                                           '#include <tools/harness/test.h>\n')), 1)
        self.assertEqual(len(self.problems('lib/dom/quickjs/src/x.cpp',
                                           '#include "quickjs.h"\n')), 0)
        self.assertEqual(len(self.problems('lib/dom/src/x.cpp', '#include "quickjs.h"\n')), 1)
        self.assertEqual(len(self.problems('lib/core/src/x.cpp', '#include "quickjs.h"\n')), 1)

    def test_doctest_is_confined_to_tests_through_the_harness(self):
        self.assertEqual(len(self.problems('lib/draw/backends/cpu/tests/test_x.cpp',
                                           '#include "tools/harness/test.h"\n')), 0)
        self.assertEqual(len(self.problems('lib/draw/src/x.cpp', '#include "tools/harness/test.h"\n')), 1)
        self.assertEqual(len(self.problems('lib/draw/backends/cpu/tests/test_x.cpp',
                                           '#include "doctest/doctest.h"\n')), 1)

    def test_a_directive_separates_includes(self):
        self.assertEqual(self.problems('lib/core/src/x.cpp',
                                       '#include "core/src/b.h"\n#if X\n'
                                       '#include "core/src/a.h"\n#endif\n'), [])


class CommentColumns(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_comment_columns(report, 'x.h', text)
        return [(line, rule) for _, line, rule, _ in report.problems]

    def test_a_continuation_starts_where_its_comment_does(self):
        self.assertEqual(self.problems('    int a;   // one\n'
                                       '             // two\n'), [])
        self.assertEqual(self.problems('    int a;   // one\n'
                                       '           // two\n'), [(2, 'comment-column')])

    def test_a_comment_at_the_code_indent_starts_anew(self):
        self.assertEqual(self.problems('    int a;   // one\n'
                                       '    // about b\n'
                                       '    int b;\n'), [])

    def test_comments_beside_consecutive_code_share_a_column(self):
        self.assertEqual(self.problems('    int a;      // one\n'
                                       '    int bb;     // two\n'), [])
        self.assertEqual(self.problems('    int a;      // one\n'
                                       '    int bb;   // two\n'
                                       '    int c;      // three\n'), [(2, 'comment-align')])

    def test_code_that_reaches_the_column_moves_the_run_one_space_past_it(self):
        self.assertEqual(self.problems('    int a;         // one\n'
                                       '    int long_name; // two\n'
                                       '    int b;         // three\n'), [])
        self.assertEqual(self.problems('    int a;    // one\n'
                                       '    int long_name; // two\n'
                                       '    int b;    // three\n'), [(1, 'comment-align'), (3, 'comment-align')])
        self.assertEqual(self.problems('    int a;         // one\n'
                                       '    int long_name;  // two\n'), [(2, 'comment-align')])

    def test_a_blank_line_or_another_indentation_starts_a_new_run(self):
        self.assertEqual(self.problems('    int a;    // one\n'
                                       '\n'
                                       '    int b;  // two\n'), [])
        self.assertEqual(self.problems('    int a;    // one\n'
                                       '        int b;  // two\n'), [])

    def test_a_comment_marker_in_a_string_is_code(self):
        self.assertEqual(self.problems('    f("//");\n'
                                       '        // unrelated\n'), [])


class Lifetime(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        path = os.path.join(lint.ROOT, 'lib/dom/include/pr/dom_example.h')
        lint.check_lifetime(report, [path], {path: text})
        return [message for _, _, _, message in report.problems]

    def test_a_type_is_destroyed_or_retained_and_released(self):
        self.assertEqual(self.problems('void prDomThingDestroy(PrDomThing* self);\n'
                                       'void prDomObserverRetain(PrDomObserver* self);\n'
                                       'void prDomObserverRelease(PrDomObserver* self);\n'), [])

    def test_destroy_does_not_go_with_retain(self):
        self.assertEqual(self.problems('void prDomThingDestroy(PrDomThing* self);\n'
                                       'void prDomThingRetain(PrDomThing* self);\n'),
                         ["prDomThing has Destroy and Retain: a type has Destroy, or Retain and "
                          "Release"])

    def test_retain_needs_release(self):
        self.assertEqual(self.problems('void prDomRangeRetain(PrDomRange* self);\n'),
                         ["prDomRange has Retain alone: Retain and Release come together"])

    def test_other_verbs_are_rejected(self):
        self.assertEqual(self.problems('void prDomRangeFree(PrDomRange* self);\n'),
                         ["prDomRangeFree is no lifetime function: a type has Destroy, or Retain "
                          "and Release"])

    def test_freeing_memory_is_not_a_lifetime_function(self):
        self.assertEqual(self.problems('void prMemoryFree(void* PR_NULLABLE memory);\n'), [])

    def test_internal_types_follow_the_same_vocabulary(self):
        self.assertEqual(self.problems('void domRangeRetain(DomRange* range);\n'),
                         ["domRange has Retain alone: Retain and Release come together"])


class BackendNames(unittest.TestCase):
    INTERFACE = 'lib/draw/include/pr/draw_renderer.h'

    def problems(self, interface, backend):
        report = lint.Report()
        paths = {os.path.join(lint.ROOT, self.INTERFACE): interface,
                 os.path.join(lint.ROOT, 'lib/draw/backends/cpu/include/pr/draw_cpu.h'): backend}
        lint.check_backend_names(report, list(paths), paths)
        return [message for _, _, _, message in report.problems]

    def test_a_backend_constructor_names_its_interface_and_backend(self):
        interface = 'void prDrawRendererDestroy(PrDrawRenderer* renderer);\n'
        self.assertEqual(self.problems(interface, 'PrDrawRenderer* prDrawCpuCreate(void);\n'), [])
        self.assertEqual(self.problems(interface, 'PrDrawRenderer* prDrawCpuBackend(void);\n'),
                         ["prDrawCpuBackend makes a PrDrawRenderer, so it is named like prdrawcpucreate"])

    def test_a_record_of_functions_is_an_interface(self):
        interface = 'struct PrDrawRenderer {\n    void (*PR_NULLABLE destroyFn)(PrDrawRenderer* renderer);\n};\n'
        self.assertEqual(self.problems(interface, 'PrDrawRenderer* prDrawCpuRenderer(void);\n'),
                         ["prDrawCpuRenderer makes a PrDrawRenderer, so it is named like prdrawcpucreate"])

    def test_a_backend_of_a_backend_names_both(self):
        report = lint.Report()
        paths = {os.path.join(lint.ROOT, 'lib/draw/backends/fragment/include/pr/draw_fragment.h'):
                 'void prDrawFragmentDriverDestroy(PrDrawFragmentDriver* driver);\n',
                 os.path.join(lint.ROOT, 'lib/draw/backends/fragment_metal/include/pr/draw_fragment_metal.h'):
                 'PrDrawFragmentDriver* prDrawFragmentMetalCreate(void);\n'
                 'PrDrawFragmentDriver* prDrawMetalCreate(void);\n'
                 'bool prDrawMetalDriverCreate(PrDrawFragmentDriver* PR_NULLABLE* PR_NONNULL out);\n'}
        lint.check_backend_names(report, list(paths), paths)
        self.assertEqual([message for _, _, _, message in report.problems],
                         ["prDrawMetalCreate makes a PrDrawFragmentDriver, so it is named like "
                          "prdrawfragmentmetalcreate",
                          "prDrawMetalDriverCreate makes a PrDrawFragmentDriver, so it is named like "
                          "prdrawfragmentmetalcreate"])

    def test_no_type_is_named_backend(self):
        self.assertEqual(self.problems('typedef struct PrDrawBackend PrDrawBackend;\n', ''),
                         ["PrDrawBackend is named for its role, not Backend"])


class Creators(unittest.TestCase):
    def problems(self, text, path='lib/lottie/include/pr/lottie.h'):
        report = lint.Report()
        full = os.path.join(lint.ROOT, path)
        lint.check_creators(report, [full], {full: text})
        return [message for _, _, _, message in report.problems]

    def test_a_create_names_the_handle_it_makes(self):
        header = ('PrStatus prLottieAnimationCreate(PrStringView json, PrLottieAnimation* PR_NULLABLE* PR_NONNULL out);\n'
                  'void prLottieAnimationDestroy(PrLottieAnimation* PR_NULLABLE animation);\n'
                  'PrLottieArchive* prLottieArchiveCreate(void);\n'
                  'void prLottieArchiveDestroy(PrLottieArchive* PR_NULLABLE archive);\n')
        self.assertEqual(self.problems(header), [])
        self.assertEqual(self.problems(header + 'PrStatus prLottieArchiveCreateAnimation(const PrLottieArchive* archive, '
                                                'PrLottieAnimation* PR_NULLABLE* PR_NONNULL out);\n'),
                         ["prLottieArchiveCreateAnimation makes a PrLottieAnimation, so it is named "
                          "prLottieAnimationCreate..."])

    def test_a_member_that_makes_a_handle_is_its_create(self):
        header = ('typedef struct PrDomDocument PrDomDocument;\ntypedef struct PrDomElement PrDomElement;\n'
                  'PrStatus prDomElementCreate(PrDomDocument* document, PrStringView localName, '
                  'PrDomElement* PR_NULLABLE* PR_NONNULL out);\n')
        self.assertEqual(self.problems(header, 'lib/dom/include/pr/dom.h'), [])
        self.assertEqual(self.problems(header.replace('prDomElementCreate', 'prDomDocumentCreateElement'),
                                       'lib/dom/include/pr/dom.h'),
                         ["prDomDocumentCreateElement makes a PrDomElement, so it is named prDomElementCreate..."])

    def test_an_implementation_of_an_interface_extends_its_name(self):
        self.assertEqual(self.problems('typedef struct PrStorage PrStorage;\nPrStorage* prStorageDiskCreate(void);\n',
                                       'lib/storage/include/pr/storage.h'), [])

    def test_a_destroyed_handle_has_a_create(self):
        header = ('PrStatus prDrawRendererPrepare(PrDrawRenderer* renderer, PrDrawFrame* PR_NULLABLE* PR_NONNULL out);\n'
                  'void prDrawFrameDestroy(PrDrawFrame* PR_NULLABLE frame);\n')
        self.assertEqual(self.problems(header, 'lib/draw/include/pr/draw_renderer.h'),
                         ["PrDrawFrame has Destroy but no Create or Begin that makes it: an operation that produces one "
                          "fills a PrDrawFrame that its caller made and reuses"])
        self.assertEqual(self.problems('PrDrawFrame* prDrawFrameCreate(PrDrawRenderer* renderer);\n'
                                       'void prDrawFrameDestroy(PrDrawFrame* PR_NULLABLE frame);\n',
                                       'lib/draw/include/pr/draw_renderer.h'), [])

    def test_a_record_is_made_by_its_backends(self):
        header = ('struct PrDrawRenderer {\n    void (*PR_NULLABLE destroyFn)(PrDrawRenderer* renderer);\n};\n'
                  'void prDrawRendererDestroy(PrDrawRenderer* PR_NULLABLE renderer);\n')
        self.assertEqual(self.problems(header, 'lib/draw/include/pr/draw_renderer.h'), [])


class Tags(unittest.TestCase):
    def problems(self, files):
        report = lint.Report()
        paths = {os.path.join(lint.ROOT, path): text for path, text in files.items()}
        lint.check_tags(report, list(paths), paths)
        return [message for _, _, _, message in report.problems]

    def test_a_name_has_a_tag_that_its_module_owns(self):
        self.assertEqual(self.problems({'lib/compress/include/pr/compress_zip.h':
                                         'typedef struct PrCompressZip PrCompressZip;\n'
                                         'void prCompressZipClose(PrCompressZip* zip);\n'
                                         'enum { kPrCompressMaxLevel = 9 };\n#define PR_COMPRESS_LEVEL 6\n'}), [])
        self.assertEqual(self.problems({'lib/compress/include/pr/compress_zip.h':
                                         'typedef struct PrZipArchive PrZipArchive;\n#define PR_ZIP_LEVEL 6\n'}),
                         ["PR_ZIP_LEVEL begins with no module's tag; lib/compress owns Compress",
                          "PrZipArchive begins with no module's tag; lib/compress owns Compress"])

    def test_a_tag_is_a_whole_word(self):
        self.assertEqual(self.problems({'lib/fetch/include/pr/fetch.h': 'typedef struct PrFetcher PrFetcher;\n'}),
                         ["PrFetcher begins with no module's tag; lib/fetch owns Fetch"])

    def test_a_name_is_read_by_its_longest_tag(self):
        files = {'lib/font/include/pr/font.h': 'void prFontBundlePush(int depth);\n',
                 'lib/font_bundle/include/pr/font_bundle.h': 'void prFontBundlePop(int depth);\n'}
        self.assertEqual(self.problems(files), ["prFontBundlePush has the tag FontBundle, which lib/font_bundle owns"])

    def test_a_backend_uses_its_interfaces_tag_and_core_its_macros(self):
        self.assertEqual(self.problems({'lib/draw/backends/cpu/include/pr/draw_cpu.h':
                                         'PrDrawRenderer* prDrawCpuCreate(void);\n',
                                         'lib/core/include/pr/base.h': '#define PR_NULLABLE _Nullable\n'}), [])


class SharedIncludes(unittest.TestCase):
    def problems(self, path, text):
        report = lint.Report()
        lint.check_shared_includes(report, os.path.join(lint.ROOT, path), text)
        return [message for _, _, _, message in report.problems]

    def test_only_an_interfaces_backends_include_what_they_share(self):
        include = '#include "pr/draw_shared_atlas.h"\n'
        self.assertEqual(self.problems('lib/draw/backends/fragment/src/draw_fragment.cpp', include), [])
        self.assertEqual(self.problems('lib/draw/comparisons/blend2d/src/draw_blend2d.cpp', include), [])
        self.assertEqual(self.problems('lib/layout/src/layout_paint.cpp', include),
                         ["pr/draw_shared_atlas.h is shared by the backends of lib/draw, and only they include it"])


class Wrappers(unittest.TestCase):
    def test_each_group_of_wrappers_is_sorted_by_path(self):
        # The groups are the runs of entries between the table's comments, which lint.py's source holds.
        text = Path(lint.__file__).read_text(encoding="utf-8")
        body = text[text.index("WRAPPERS = {\n"):text.index("\n}\n", text.index("WRAPPERS = {\n"))]
        groups = [[]]
        for line in body.split("\n")[1:]:
            if line.strip().startswith("#"):
                groups.append([])
            elif line.startswith('    "'):
                groups[-1].append(line.split('"')[1])
        for paths in groups:
            self.assertEqual(paths, sorted(paths))
        self.assertEqual(sum(len(paths) for paths in groups), len(lint.WRAPPERS))


class BackendHeaders(unittest.TestCase):
    def problems(self, path, text):
        report = lint.Report()
        lint.check_backend_headers(report, os.path.join(lint.ROOT, path), text, {"draw_cpu.h": "lib/draw/"})
        return [message for _, _, _, message in report.problems]

    def test_only_an_interfaces_module_includes_its_backends(self):
        include = '#include "pr/draw_cpu.h"\n'
        self.assertEqual(self.problems('lib/draw/src/draw_renderer.cpp', include), [])
        self.assertEqual(self.problems('lib/draw/backends/cpu/src/draw_cpu.cpp', include), [])
        self.assertEqual(self.problems('lib/canvas/src/canvas.cpp', '#include "pr/draw_renderer.h"\n'), [])
        self.assertEqual(self.problems('lib/canvas/src/canvas.cpp', include),
                         ["pr/draw_cpu.h is a backend of lib/draw; use its interface"])

    def test_the_backend_headers_are_found_in_the_tree(self):
        headers = lint.backend_headers()
        self.assertEqual(headers.get("draw_cpu.h"), "lib/draw/")
        self.assertEqual(headers.get("core_windows_file.h"), "lib/core/")
        self.assertNotIn("draw_shared_atlas.h", headers)
        # A platform's helpers are core's, not a backend's.
        self.assertNotIn("windows_string.h", headers)

    def test_another_librarys_backend_does_not_include_cores_backends(self):
        report = lint.Report()
        path = os.path.join(lint.ROOT, 'lib/app/backends/win32/src/win32_browser.cpp')
        lint.check_backend_headers(report, path, '#include "pr/core_windows_file.h"\n#include "pr/windows_string.h"\n',
                                   lint.backend_headers())
        self.assertEqual([message for _, _, _, message in report.problems],
                         ["pr/core_windows_file.h is a backend of lib/core; use its interface"])


class SharedImplementation(unittest.TestCase):
    def private(self, path, text):
        report = lint.Report()
        lint.check_private_includes(report, os.path.join(lint.ROOT, path), text)
        return [rule for _, _, rule, _ in report.problems]

    def test_a_backend_reaches_what_its_interfaces_backends_share(self):
        include = '#include "tls/backends/shared/src/tls_shared.h"\n'
        self.assertEqual(self.private('lib/tls/backends/openssl/src/tls_openssl.cpp', include), [])
        self.assertEqual(self.private('lib/http/backends/builtin/src/http_builtin.cpp', include), ['private-include'])

    def test_a_header_that_each_platform_implements_pairs_with_each_source(self):
        header = 'lib/net/backends/sockets/src/net_sockets_platform.h'
        sources = ['lib/net/backends/sockets/src/net_sockets_platform_posix.cpp',
                   'lib/net/backends/sockets/src/net_sockets_platform_windows.cpp']
        texts = {os.path.join(lint.ROOT, header): 'void socketsClose(SocketsHandle socket);\n'}
        texts.update({os.path.join(lint.ROOT, s): 'void socketsClose(SocketsHandle socket) {\n}\n' for s in sources})
        report = lint.Report()
        lint.check_homes(report, [os.path.join(lint.ROOT, header)], [os.path.join(lint.ROOT, s) for s in sources],
                         texts)
        self.assertEqual(report.problems, [])


class ModuleWords(unittest.TestCase):
    def problems(self, path, text):
        report = lint.Report()
        path = os.path.join(lint.ROOT, path)
        lint.check_module_words(report, path, text)
        return [message for _, _, _, message in report.problems]

    def test_an_internal_name_begins_with_its_modules_word(self):
        self.assertEqual(self.problems('lib/layout/src/layout_box.h',
                                       'struct LayoutBox {};\nLayoutBox* layoutBoxCreate(void);\n'), [])
        self.assertEqual(self.problems('lib/layout/src/layout_box.h', 'struct Box {};\nvoid shade(void);\n'),
                         ["shade begins with none of its module's words: layout",
                          "Box begins with none of its module's words: layout"])

    def test_a_backend_a_format_and_a_binding_own_words(self):
        self.assertEqual(self.problems('lib/draw/backends/cpu/src/cpu_pixels.h', 'struct CpuPixel {};\n'), [])
        self.assertEqual(self.problems('lib/font/src/sfnt.h', 'uint32_t sfntAxisCount(const Sfnt* sfnt);\n'), [])
        self.assertEqual(self.problems('lib/url/quickjs/src/url_quickjs.h', 'void jsUrlInit(void);\n'), [])


class FunctionNames(unittest.TestCase):
    def problems(self, path, text):
        report = lint.Report()
        path = os.path.join(lint.ROOT, path)
        lint.check_function_names(report, [path], {path: text})
        return [message for _, _, _, message in report.problems]

    def test_a_lookup_is_from_its_source(self):
        self.assertEqual(self.problems('lib/locale/include/pr/locale.h',
                                       'PrLocale prLocaleFromTag(PrStringView tag);\n'), [])
        self.assertEqual(self.problems('lib/locale/include/pr/locale.h',
                                       'PrLocale prLocaleOf(PrStringView tag);\n'
                                       'PrFontFamily prFontFamilyNamed(PrStringView name);\n'),
                         ["prLocaleOf ends in Of: a lookup from another form is ...From<Source>, and a getter is "
                          "named for what it returns",
                          "prFontFamilyNamed ends in Named: a lookup from another form is ...From<Source>, and a "
                          "getter is named for what it returns"])

    def test_only_a_web_idl_member_has_get(self):
        self.assertEqual(self.problems('lib/dom/include/pr/dom.h',
                                       'PrDomElement* prDomNodeGetElementById(PrDomNode* self, PrStringView id);\n'),
                         [])
        self.assertEqual(self.problems('lib/gpu/include/pr/gpu.h',
                                       'PrGpuTextureDesc prGpuTextureGetDesc(PrGpuTexture texture);\n'),
                         ["prGpuTextureGetDesc has Get: a getter is named for what it returns"])


class HeaderNames(unittest.TestCase):
    def problems(self, path, text):
        report = lint.Report()
        path = os.path.join(lint.ROOT, path)
        lint.check_header_names(report, [path], {path: text})
        return [message for _, _, _, message in report.problems]

    def test_a_public_header_is_named_for_its_tag(self):
        self.assertEqual(self.problems('lib/draw/include/pr/draw_list.h', 'void prDrawListClear(PrDrawList* l);\n'), [])
        self.assertEqual(self.problems('lib/draw/include/pr/list.h', 'void prDrawListClear(PrDrawList* l);\n'),
                         ["list.h declares no PrList names, the tag its name starts with"])

    def test_an_untagged_module_names_headers_freely(self):
        self.assertEqual(self.problems('lib/core/include/pr/base.h', 'typedef struct PrStringView PrStringView;\n'),
                         [])

    def test_a_backend_header_starts_with_its_directories(self):
        text = 'PrDrawRenderer* prDrawCpuCreate(void);\n'
        self.assertEqual(self.problems('lib/draw/backends/cpu/include/pr/draw_cpu.h', text), [])
        self.assertEqual(self.problems('lib/draw/backends/cpu/include/pr/draw_raster.h', text),
                         ["a header of lib/draw/backends/cpu is named draw_cpu_..."])


class UniqueNames(unittest.TestCase):
    def problems(self, paths):
        report = lint.Report()
        lint.check_unique_names(report, [os.path.join(lint.ROOT, p) for p in paths])
        return [message for _, _, _, message in report.problems]

    def test_a_source_shares_its_name_with_its_headers(self):
        self.assertEqual(self.problems(['lib/draw/include/pr/draw_list.h', 'lib/draw/src/draw_list.h',
                                        'lib/draw/src/draw_list.cpp']), [])

    def test_two_modules_never_share_a_name(self):
        self.assertEqual(self.problems(['lib/draw/tests/test_text.cpp', 'lib/text/tests/test_text.cpp']),
                         ["test_text.cpp shares its name with lib/draw/tests/test_text.cpp"])

    def test_a_binding_and_a_source_never_share_a_name(self):
        self.assertEqual(self.problems(['lib/dom/quickjs/src/dom.cpp', 'lib/dom/src/dom.cpp']),
                         ["dom.cpp shares its name with lib/dom/quickjs/src/dom.cpp"])


class Indentation(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_indentation(report, 'x.cpp', text)
        return [line for _, line, _, _ in report.problems]

    def test_a_continuation_is_one_level_past_the_line_it_continues(self):
        self.assertEqual(self.problems('void f(int a,\n    int b);\n'), [])
        self.assertEqual(self.problems('void f(int a,\n       int b);\n'), [2])
        self.assertEqual(self.problems('void f() {\n    bool ok = a\n        && b;\n}\n'), [])

    def test_a_closing_bracket_returns_to_its_opening_line(self):
        self.assertEqual(self.problems('void f() {\n    g(\n        a,\n        b\n    );\n}\n'), [])
        self.assertEqual(self.problems('void f() {\n    g(\n        a\n        );\n}\n'), [4])

    def test_a_lambda_body_is_one_level_past_the_line_that_opens_it(self):
        self.assertEqual(self.problems('void f() {\n    g(a, [&] {\n        h();\n    }, b);\n}\n'), [])
        self.assertEqual(self.problems('void f() {\n    g(a,\n        [&] {\n            h();\n        });\n}\n'),
                         [])
        self.assertEqual(self.problems('void f() {\n    g(a, [&] {\n            h();\n        }, b);\n}\n'), [3, 4])

    def test_an_initializer_list_is_data(self):
        self.assertEqual(self.problems('void f() {\n    PrX x{\n        .a = 1,\n        .b = 2,\n    };\n}\n'), [])
        self.assertEqual(self.problems('enum {\n    A,\n    B,\n};\n'), [])

    def test_blocks_switches_and_linkage(self):
        self.assertEqual(self.problems('void f() {\n    switch (x) {\n        case 1:\n            g();\n'
                                       '            break;\n    }\n}\n'), [])
        self.assertEqual(self.problems('extern "C" {\nvoid f();\n}\n'), [])
        self.assertEqual(self.problems('PR_ASSUME_NONNULL_BEGIN\nvoid f();\n'), [])

    def test_a_raw_string_keeps_its_text(self):
        self.assertEqual(self.problems('const char* k = R"(\n  text\n)";\nvoid f();\n'), [])

    def test_tabs_are_not_indentation(self):
        self.assertEqual(self.problems('void f() {\n\tg();\n}\n'), [2, 2])

    def test_a_template_header_with_a_requires_clause_begins_its_declaration(self):
        self.assertEqual(self.problems('template <typename T> requires requires { typename T::U; }\nstruct S<T> {\n'
                                       '    int x;\n};\n'), [])

    def test_a_body_after_a_member_initialized_by_a_list_ends_the_statement(self):
        self.assertEqual(self.problems('struct S : B {\n    S(int* p) :\n            B{.p = p} {}\n    ~S() {}\n};\n'), [])

    def test_a_continued_item_is_one_level_past_the_item(self):
        self.assertEqual(self.problems('X x{\n    .a = "b"\n        "c",\n    .d = 1,\n};\n'), [])
        self.assertEqual(self.problems('f("a"\n    "b"\n    "c");\n'), [])


class Ternaries(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_ternaries(report, 'x.cpp', text)
        return [line for _, line, _, _ in report.problems]

    def test_a_conditional_on_one_line_passes(self):
        self.assertEqual(self.problems('int f(bool a) {\n    int x = a ? 1 : 2;\n    return a ? x : 3;\n}\n'), [])
        self.assertEqual(self.problems('void f() {\n    if (a) {\n        g();\n    }\n    x += a ? 1 : 2;\n}\n'), [])
        self.assertEqual(self.problems('int f() {\n    if (a)\n        return b ? 1 : 2;\n    return 0;\n}\n'), [])
        self.assertEqual(self.problems('void f() {\n    g(a,\n        b ? 1 : 2);\n}\n'), [])

    def test_a_conditional_that_spans_lines_is_flagged_at_its_condition(self):
        self.assertEqual(self.problems('int f() {\n    return a ?\n        1 : 2;\n}\n'), [2])
        self.assertEqual(self.problems('void f() {\n    g(a,\n        b ? 1 :\n            c ? 2 : 3);\n}\n'), [3])
        self.assertEqual(self.problems('int x = a\n    && b ? 1 : 2;\n'), [1])


class LineBreaks(unittest.TestCase):
    def problems(self, text):
        return [(number, check) for number, check, _, _ in lint.layout_problems(text)]

    def laid_out(self, text):
        return layout.laid_out_text(text)

    def test_a_line_that_fits_on_the_line_before_joins_it(self):
        self.assertEqual(self.problems('void f() {\n    g(a,\n        b);\n}\n'), [(3, 'wrap')])
        self.assertEqual(self.laid_out('for (int i = 0;\n    i < n;) {\n}\n'), 'for (int i = 0; i < n;) {\n}\n')

    def test_a_line_that_does_not_fit_stays(self):
        text = 'void f() {\n    g(' + 'a' * 112 + ',\n        ' + 'b' * 20 + ');\n}\n'
        self.assertEqual(self.problems(text), [])

    def test_a_short_tail_joins_the_line_before_past_the_limit(self):
        text = 'void f() {\n    g(' + 'a' * 112 + ',\n        b);\n}\n'
        self.assertEqual(self.problems(text), [(3, 'wrap')])
        report = lint.Report()
        lint.check_line_length(report, 'x.cpp', 'g(' + 'a' * 117 + ', b);\n', ())
        self.assertEqual(report.problems, [])

    def test_a_wrapped_head_is_indented_past_its_body(self):
        indented = layout.indented_text('void f(int a,\n    int b) {\n    g();\n    if (a &&\n        b)\n        h();\n}\n')
        self.assertEqual(indented, 'void f(int a,\n        int b) {\n    g();\n    if (a &&\n            b)\n        h();\n}\n')

    def test_a_statement_continues_past_an_initializer_that_ends_a_line(self):
        self.assertEqual(layout.indented_text('void f() {\n    p = PrX{a}\n    + q;\n}\n'),
                         'void f() {\n    p = PrX{a}\n        + q;\n}\n')

    def test_a_body_and_an_item_and_a_commented_line_stay(self):
        self.assertEqual(self.problems('void f() {\n    if (a)\n        b();\n}\n'), [])
        self.assertEqual(self.problems('int k[] = {\n    1,\n    2, // two\n    3,\n    ' + '4, ' * 40 + '\n};\n'), [])
        self.assertEqual(self.problems('void f() {\n    g(a, // a\n        b);\n}\n'), [])

    def test_an_operator_ends_the_line_it_breaks(self):
        self.assertEqual(self.laid_out('bool b = c\n    && d(' + 'e' * 120 + ');\n'),
                         'bool b = c &&\n    d(' + 'e' * 120 + ');\n')

    def test_an_operator_runs_past_the_limit_as_a_short_tail(self):
        long = 'x' * 104
        tail = 'd' * 20
        self.assertEqual(self.laid_out('bool b = c && ' + long + '\n    && ' + tail + ';\n'),
                         'bool b = c && ' + long + ' &&\n    ' + tail + ';\n')

    def test_a_line_that_runs_too_long_breaks_where_it_nests_least(self):
        a, b, c = 'a' * 50, 'b' * 40, 'c' * 50
        self.assertEqual(self.laid_out('bool x = f(' + a + ' == 1 || ' + b + ' == 2) && ' + c + ';\n'),
                         'bool x = f(' + a + ' == 1 || ' + b + ' == 2) &&\n    ' + c + ';\n')

    def test_a_line_breaks_before_a_literal_and_keeps_it(self):
        call = '    f(' + 'a' * 100 + ', &color, "green", false);\n'
        self.assertEqual(self.laid_out('void g() {\n' + call + '}\n'),
                         'void g() {\n    f(' + 'a' * 100 + ', &color,\n        "green", false);\n}\n')

    def test_a_continued_string_joins_as_one_literal(self):
        self.assertEqual(self.laid_out('const char* s = "a"\n    "b";\n'), 'const char* s = "ab";\n')
        self.assertEqual(self.laid_out('const char* s = "a\\n"\n    "b";\n'), 'const char* s = "a\\n"\n    "b";\n')
        self.assertEqual(self.laid_out('const char* s = "\\x1"\n    "2";\n'), 'const char* s = "\\x1" "2";\n')

    def test_an_initializer_that_fits_is_one_line(self):
        self.assertEqual(self.laid_out('X x{\n    .a = 1,\n    .b = 2,\n};\n'), 'X x{.a = 1, .b = 2};\n')

    def test_a_designated_initializer_that_wraps_has_an_item_a_line(self):
        long = 'y' * 60
        self.assertEqual(self.laid_out('X x{.a = ' + long + ',\n    .b = ' + long + ', .c = 1};\n'),
                         'X x{\n    .a = ' + long + ',\n    .b = ' + long + ',\n    .c = 1,\n};\n')

    def test_a_positional_initializer_that_wraps_keeps_its_rows(self):
        row = ', '.join(['{512, 2048}'] * 9)
        self.assertEqual(self.laid_out('int k[][2] = {' + row + ',\n    ' + row + '};\n'),
                         'int k[][2] = {\n    ' + row + ',\n    ' + row + ',\n};\n')


class BlankLines(unittest.TestCase):
    def problems(self, text):
        report = lint.Report()
        lint.check_blank_lines(report, 'x.cpp', text)
        return [line for _, line, _, _ in report.problems]

    def test_one_blank_line_at_most(self):
        self.assertEqual(self.problems('int a;\n\nint b;\n'), [])
        self.assertEqual(self.problems('int a;\n\n\nint b;\n'), [3])


class Homes(unittest.TestCase):
    def problems(self, files):
        report = lint.Report()
        paths = {os.path.join(lint.ROOT, p): t for p, t in files.items()}
        headers = [p for p in paths if p.endswith('.h')]
        sources = [p for p in paths if not p.endswith('.h')]
        lint.check_homes(report, headers, sources, paths)
        return [message for _, _, _, message in report.problems]

    def test_a_header_pairs_with_its_source(self):
        self.assertEqual(self.problems({
            'lib/draw/include/pr/draw_list.h': 'void prDrawListClear(PrDrawList* list);\n',
            'lib/draw/src/draw_list.cpp': 'void prDrawListClear(PrDrawList* list) {\n}\n',
            'lib/draw/src/freeze.h': 'void drawListFreeze(PrDrawList* list);\n',
            'lib/draw/src/freeze.cpp': 'void drawListFreeze(PrDrawList* list) {\n}\n'}), [])

    def test_a_definition_elsewhere_is_reported(self):
        self.assertEqual(self.problems({
            'lib/draw/src/freeze.h': 'void drawListFreeze(PrDrawList* list);\n',
            'lib/draw/src/draw_list.cpp': 'void drawListFreeze(PrDrawList* list) {\n}\n'}),
            ["drawListFreeze is defined in draw_list, not freeze"])

    def test_a_generated_source_pairs_with_the_header_it_extends(self):
        self.assertEqual(self.problems({
            'lib/core/src/unicode.h': 'uint8_t prUnicodeScript(uint32_t c);\n',
            'lib/core/src/unicode_data.cpp': '// Generated by gen.py -- do not edit.\n'
                                             'uint8_t prUnicodeScript(uint32_t c) {\n}\n'}), [])


class SpecReferences(unittest.TestCase):
    IDS = {"css-syntax-3": {"consume-escaped-code-point", "serialization"},
           "cssom-1": {"update-style-attribute-for"},
           "selectors-4": {"unknown--webkit--pseudo-elements"},
           "html": {"data-state", "attribute-value-(double-quoted)-state", "document.write()",
                    "update-a-select's-selectedcontent"}}

    def problems(self, text):
        report = lint.Report()
        lint.check_spec_references(report, os.path.join(lint.ROOT, 'lib/dom/src/example.cpp'),
                                   text, self.IDS)
        return [message for _, _, _, message in report.problems]

    def test_an_id_the_spec_has(self):
        self.assertEqual(self.problems('// css-syntax-3/#consume-escaped-code-point, then '
                                       'cssom-1/#update-style-attribute-for.\n'), [])

    def test_an_id_the_spec_does_not_have(self):
        self.assertEqual(self.problems('// (cssom-1/#update-the-style-attribute)\n'),
                         ['cssom-1 has no passage #update-the-style-attribute'])

    def test_a_shortname_webref_does_not_extract(self):
        self.assertEqual(self.problems('// cssom/#update-style-attribute-for\n'),
                         ['cssom is not a spec webref extracts'])

    def test_an_id_with_parentheses(self):
        self.assertEqual(self.problems('// html/#attribute-value-(double-quoted)-state and '
                                       '(html/#document.write()).\n'), [])

    def test_an_id_with_an_apostrophe(self):
        self.assertEqual(self.problems("// html/#update-a-select's-selectedcontent, and "
                                       "html/#data-state's output\n"), [])

    def test_an_id_with_a_double_hyphen(self):
        self.assertEqual(self.problems('// (selectors-4/#unknown--webkit--pseudo-elements)\n'), [])

    def test_an_id_inside_a_string_literal(self):
        self.assertEqual(self.problems('PR_SV("html/#data-state rewrites it")\n'), [])

    def test_a_url(self):
        self.assertEqual(self.problems('// https://html.spec.whatwg.org/#data-state\n'), [])

    def test_a_number_or_a_name_in_place_of_an_id(self):
        for text in ('// css-syntax-3 4.3.7\n',
                     '// css-syntax-3 "Consume an escaped code point"\n'):
            with self.subTest(text=text):
                self.assertEqual(len(self.problems(text)), 1)

    def test_a_number_after_a_word_that_is_no_spec(self):
        self.assertEqual(self.problems('// version 4.2 of "something"\n'), [])


class Align(unittest.TestCase):
    def test_comments_beside_a_run_move_one_space_past_its_longest_code(self):
        self.assertEqual(rewrap.align('    int a;    // one\n'
                                      '    int long_name; // two\n'
                                      '    int b;      // three\n'),
                         '    int a;         // one\n'
                         '    int long_name; // two\n'
                         '    int b;         // three\n')

    def test_a_moved_comment_takes_its_continuation_with_it(self):
        self.assertEqual(rewrap.align('    int a;  // one\n'
                                      '            // more\n'
                                      '    int b;    // two\n'
                                      '    int c;    // three\n'),
                         '    int a;    // one\n'
                         '              // more\n'
                         '    int b;    // two\n'
                         '    int c;    // three\n')

    def test_a_moved_comment_that_passes_the_limit_is_refilled(self):
        words = ' '.join(['word'] * 22)
        text = rewrap.align('    int a;    // ' + words + '\n    int long_name_here; // two\n')
        lines = text.split('\n')
        self.assertTrue(all(len(line) <= lint.LINE_LIMIT for line in lines))
        self.assertEqual(lines[1], ' ' * 24 + '// word word word word')
        self.assertEqual(lines[2], '    int long_name_here; // two')


if __name__ == '__main__':
    unittest.main()
