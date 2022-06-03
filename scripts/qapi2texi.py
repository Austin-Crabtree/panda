#!/usr/bin/env python
# QAPI texi generator
#
# This work is licensed under the terms of the GNU LGPL, version 2+.
# See the COPYING file in the top-level directory.
"""This script produces the documentation of a qapi schema in texinfo format"""
from __future__ import print_function
import re
import sys

import qapi

MSG_FMT = """
@deftypefn {type} {{}} {name}

{body}
@end deftypefn

""".format

TYPE_FMT = """
@deftp {{{type}}} {name}

{body}
@end deftp

""".format

EXAMPLE_FMT = """@example
{code}
@end example
""".format


def subst_strong(doc):
    """Replaces *foo* by @strong{foo}"""
    return re.sub(r'\*([^*\n]+)\*', r'@strong{\1}', doc)


def subst_emph(doc):
    """Replaces _foo_ by @emph{foo}"""
    return re.sub(r'\b_([^_\n]+)_\b', r'@emph{\1}', doc)


def subst_vars(doc):
    """Replaces @var by @code{var}"""
    return re.sub(r'@([\w-]+)', r'@code{\1}', doc)


def subst_braces(doc):
    """Replaces {} with @{ @}"""
    return doc.replace('{', '@{').replace('}', '@}')


def texi_example(doc):
    """Format @example"""
    # TODO: Neglects to escape @ characters.
    # We should probably escape them in subst_braces(), and rename the
    # function to subst_special() or subs_texi_special().  If we do that, we
    # need to delay it until after subst_vars() in texi_format().
    doc = subst_braces(doc).strip('\n')
    return EXAMPLE_FMT(code=doc)


def texi_format(doc):
    """
    Format documentation

    Lines starting with:
    - |: generates an @example
    - =: generates @section
    - ==: generates @subsection
    - 1. or 1): generates an @enumerate @item
    - */-: generates an @itemize list
    """
    ret = ''
    doc = subst_braces(doc)
    doc = subst_vars(doc)
    doc = subst_emph(doc)
    doc = subst_strong(doc)
    inlist = ''
    lastempty = False
    for line in doc.split('\n'):
        empty = line == ''

        # FIXME: Doing this in a single if / elif chain is
        # problematic.  For instance, a line without markup terminates
        # a list if it follows a blank line (reaches the final elif),
        # but a line with some *other* markup, such as a = title
        # doesn't.
        #
        # Make sure to update section "Documentation markup" in
        # docs/qapi-code-gen.txt when fixing this.
        if line.startswith('| '):
            line = EXAMPLE_FMT(code=line[2:])
        elif line.startswith('= '):
            line = f'@section {line[2:]}'
        elif line.startswith('== '):
            line = f'@subsection {line[3:]}'
        elif re.match(r'^([0-9]*\.) ', line):
            if not inlist:
                ret += '@enumerate\n'
                inlist = 'enumerate'
            ret += '@item\n'
            line = line[line.find(' ')+1:]
        elif re.match(r'^[*-] ', line):
            if not inlist:
                ret += '@itemize %s\n' % {'*': '@bullet',
                                          '-': '@minus'}[line[0]]
                inlist = 'itemize'
            ret += '@item\n'
            line = line[2:]
        elif lastempty and inlist:
            ret += '@end %s\n\n' % inlist
            inlist = ''

        lastempty = empty
        ret += line + '\n'

    if inlist:
        ret += '@end %s\n\n' % inlist
    return ret


def texi_body(doc):
    """Format the main documentation body"""
    return texi_format(doc.body.text)


def texi_enum_value(value):
    """Format a table of members item for an enumeration value"""
    return '@item @code{%s}\n' % value.name


def texi_member(member, suffix=''):
    """Format a table of members item for an object type member"""
    typ = member.type.doc_type()
    return '@item @code{%s%s%s}%s%s\n' % (
        member.name,
        ': ' if typ else '',
        typ or '',
        ' (optional)' if member.optional else '',
        suffix,
    )


def texi_members(doc, what, base, variants, member_func):
    """Format the table of members"""
    items = ''
    for section in doc.args.values():
        # TODO Drop fallbacks when undocumented members are outlawed
        if section.text:
            desc = texi_format(section.text)
        elif (variants and variants.tag_member == section.member
              and not section.member.type.doc_type()):
            values = section.member.type.member_names()
            members_text = ', '.join(['@t{"%s"}' % v for v in values])
            desc = f'One of {members_text}' + '\n'
        else:
            desc = 'Not documented\n'
        items += member_func(section.member) + desc
    if base:
        items += '@item The members of @code{%s}\n' % base.doc_type()
    if variants:
        for v in variants.variants:
            when = ' when @code{%s} is @t{"%s"}' % (
                variants.tag_member.name, v.name)
            if v.type.is_implicit():
                assert not v.type.base and not v.type.variants
                for m in v.type.local_members:
                    items += member_func(m, when)
            else:
                items += '@item The members of @code{%s}%s\n' % (
                    v.type.doc_type(), when)
    if not items:
        return ''
    return '\n@b{%s:}\n@table @asis\n%s@end table\n' % (what, items)


def texi_sections(doc):
    """Format additional sections following arguments"""
    body = ''
    for section in doc.sections:
        name, doc = (section.name, str(section))
        func = texi_format
        if name.startswith('Example'):
            func = texi_example

        if name:
            # prefer @b over @strong, so txt doesn't translate it to *Foo:*
            body += '\n@b{%s:}\n' % section.name
        if section.name and section.name.startswith('Example'):
            body += texi_example(section.text)
        else:
            body += texi_format(section.text)
    return body


def texi_entity(doc, what, base=None, variants=None,
                member_func=texi_member):
    return (texi_body(doc)
            + texi_members(doc, what, base, variants, member_func)
            + texi_sections(doc))


class QAPISchemaGenDocVisitor(qapi.QAPISchemaVisitor):
    def __init__(self):
        self.out = None
        self.cur_doc = None

    def visit_begin(self, schema):
        self.out = ''

    def visit_enum_type(self, name, info, values, prefix):
        doc = self.cur_doc
        if self.out:
            self.out += '\n'
        self.out += TYPE_FMT(type='Enum',
                             name=doc.symbol,
                             body=texi_entity(doc, 'Values',
                                              member_func=texi_enum_value))

    def visit_object_type(self, name, info, base, members, variants):
        doc = self.cur_doc
        if base and base.is_implicit():
            base = None
        if self.out:
            self.out += '\n'
        self.out += TYPE_FMT(type='Object',
                             name=doc.symbol,
                             body=texi_entity(doc, 'Members', base, variants))

    def visit_alternate_type(self, name, info, variants):
        doc = self.cur_doc
        if self.out:
            self.out += '\n'
        self.out += TYPE_FMT(type='Alternate',
                             name=doc.symbol,
                             body=texi_entity(doc, 'Members'))

    def visit_command(self, name, info, arg_type, ret_type,
                      gen, success_response, boxed):
        doc = self.cur_doc
        if self.out:
            self.out += '\n'
        if boxed:
            body = texi_body(doc)
            body += ('\n@b{Arguments:} the members of @code{%s}\n'
                     % arg_type.name)
            body += texi_sections(doc)
        else:
            body = texi_entity(doc, 'Arguments')
        self.out += MSG_FMT(type='Command',
                            name=doc.symbol,
                            body=body)

    def visit_event(self, name, info, arg_type, boxed):
        doc = self.cur_doc
        if self.out:
            self.out += '\n'
        self.out += MSG_FMT(type='Event',
                            name=doc.symbol,
                            body=texi_entity(doc, 'Arguments'))

    def symbol(self, doc, entity):
        self.cur_doc = doc
        entity.visit(self)
        self.cur_doc = None

    def freeform(self, doc):
        assert not doc.args
        if self.out:
            self.out += '\n'
        self.out += texi_body(doc) + texi_sections(doc)


def texi_schema(schema):
    """Convert QAPI schema documentation to Texinfo"""
    gen = QAPISchemaGenDocVisitor()
    gen.visit_begin(schema)
    for doc in schema.docs:
        if doc.symbol:
            gen.symbol(doc, schema.lookup_entity(doc.symbol))
        else:
            gen.freeform(doc)
    return gen.out


def main(argv):
    """Takes schema argument, prints result to stdout"""
    if len(argv) != 2:
        print(f"{argv[0]}: need exactly 1 argument: SCHEMA", file=sys.stderr)
        sys.exit(1)

    schema = qapi.QAPISchema(argv[1])
    if not qapi.doc_required:
        print("%s: need pragma 'doc-required' "
               "to generate documentation" % argv[0], file=sys.stderr)
        sys.exit(1)
    print(texi_schema(schema))


if __name__ == '__main__':
    main(sys.argv)
