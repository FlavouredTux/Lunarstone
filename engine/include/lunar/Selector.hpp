#pragma once

// Instance selectors, the syntax of StyleRule.Selector that
// Instance:QueryDescendants takes:
//
//     Frame              a class (IsA, so abstract ones like GuiObject work)
//     .Tag  #Name        a CollectionService tag, a Name
//     [$Attr]            an attribute that's set; [$Attr = value] equal to it
//     [Prop = value]     a property equal to value (numbers, true/false, text)
//     :not(s)  :has(s)   matching none of s; with a descendant matching s
//     a > b  a >> b      b a child, a descendant of a (a space is >> too)
//     > b                a child of the instance queried
//     a, b               either
//
// Compounds join without spaces: "Frame.Tag#Name[$Open]". GUESS: other
// pseudo-classes (:hover, :press) never match outside the GUI's state.

#include <lunar/Instance.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace lunar {

// root's descendants matching the selector, in GetDescendants' order; an
// error for a selector that doesn't parse.
std::expected<std::vector<InstancePtr>, std::string> queryDescendants(const Instance& root, std::string_view selector);

} // namespace lunar
