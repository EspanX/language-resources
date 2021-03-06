// thrax_g2p.cc
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Copyright 2016, 2017 Google, Inc.
// Author: mjansche@google.com (Martin Jansche)
//
// \file
// Grapheme-to-phoneme processing with weighted grammars (typically Thrax).

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <fst/arc.h>
#include <fst/compact-fst.h>
#include <fst/compat.h>
#include <fst/compose.h>
#include <fst/project.h>
#include <fst/properties.h>
#include <fst/prune.h>
#include <fst/rmepsilon.h>
#include <fst/string.h>
#include <fst/symbol-table.h>
#include <fst/util.h>
#include <fst/vector-fst.h>
#include <thrax/grm-manager.h>

#include "absl/strings/str_split.h"
#include "festus/fst-util.h"
#include "festus/label-maker.h"

using fst::StdVectorFst;
using fst::SymbolTable;
using std::string;

DEFINE_string(fst, "", "Path to G2P FST.");
DEFINE_string(far, "", "Path to FAR file containing G2P FST.");
DEFINE_string(far_g2p_key, "G2P", "Name of G2P FST within the FAR file.");

DEFINE_string(input_labels, "UNICODE",
              "Input label type. "
              "Either \"BYTE\", or \"UNICODE\", or a SymbolTable text file.");
DEFINE_string(output_labels, "",
              "Output label type. "
              "Either \"BYTE\", or \"UNICODE\", or a SymbolTable text file.");
DEFINE_string(phoneme_syms, "", "Path to phoneme symbols. [DEPRECATED]");

DEFINE_bool(phrases, true, "Whether input lines consist of multiple phrases.");

std::unique_ptr<festus::LabelMaker> GetLabelMaker(const string &ltype) {
  std::unique_ptr<festus::LabelMaker> result;
  if (ltype.empty() || ltype == "UNICODE") {
    result.reset(new festus::UnicodeLabelMaker());
  } else if (ltype == "BYTE") {
    result.reset(new festus::ByteLabelMaker());
  } else {
    std::unique_ptr<SymbolTable> syms(SymbolTable::ReadText(ltype));
    if (syms) {
      result.reset(new festus::SymbolLabelMaker(syms.get(), " "));
    }
  }
  return result;
}

int main(int argc, char *argv[]) {
  SET_FLAGS(argv[0], &argc, &argv, true);

  std::unique_ptr<StdVectorFst> fst;
  if (!FLAGS_fst.empty()) {
    if (!FLAGS_far.empty()) {
      LOG(WARNING) << "Both --fst and --far were specified; ignoring --far";
    }
    fst.reset(StdVectorFst::Read(FLAGS_fst));
    if (!fst) return 1;
  } else if (!FLAGS_far.empty()) {
    thrax::GrmManager grm_manager;
    if (!grm_manager.LoadArchive(FLAGS_far)) {
      LOG(ERROR) << "Could not load FAR from " << FLAGS_far;
      return 1;
    }
    const auto *g2p = grm_manager.GetFst(FLAGS_far_g2p_key);
    if (!g2p) {
      LOG(ERROR) << "Could not find G2P FST with key " << FLAGS_far_g2p_key
                 << " inside FAR " << FLAGS_far;
      return 1;
    }
    fst.reset(new StdVectorFst());
    *fst = *g2p;
  } else {
    LOG(ERROR) << "Neither --fst nor --far was specified";
    return 1;
  }

  const auto input_label_maker = GetLabelMaker(FLAGS_input_labels);
  if (!input_label_maker) return 1;

  std::unique_ptr<festus::LabelMaker> output_label_maker;
  if (!FLAGS_output_labels.empty()) {
    if (!FLAGS_phoneme_syms.empty()) {
      LOG(WARNING) << "Both --output_labels and --phoneme_syms specified; "
                      "ignoring --phoneme_syms";
    }
    output_label_maker = GetLabelMaker(FLAGS_output_labels);
  } else {
    if (FLAGS_phoneme_syms.empty()) {
      LOG(ERROR) << "--phoneme_syms not specified";
      return 1;
    }
    output_label_maker = GetLabelMaker(FLAGS_phoneme_syms);
  }
  if (!output_label_maker) return 1;

  StdVectorFst lattice;
  for (string line; std::getline(std::cin, line);) {
    std::ostringstream output;
    output << line << "\t";
    bool success = true;
    bool at_start = true;
    std::vector<string> phrases;
    if (FLAGS_phrases) {
      phrases = absl::StrSplit(line, ' ');
    } else {
      phrases.emplace_back(std::move(line));
    }
    for (const auto &phrase : phrases) {
      if (at_start) {
        at_start = false;
      } else {
        output << " | ";
      }
      fst::StdCompactStringFst graphemes;
      if (!input_label_maker->StringToCompactFst(phrase, &graphemes)) {
        output << "ERROR_compiling_input: " << phrase;
        success = false;
        continue;
      }
      fst::Compose(graphemes, *fst, &lattice);
      if (fst::kNoStateId == lattice.Start()) {
        output << "ERROR_empty_composition: " << phrase;
        success = false;
        continue;
      }
      fst::Project(&lattice, fst::PROJECT_OUTPUT);
      if (lattice.Properties(fst::kString, true)) {
        VLOG(1) << "Lattice is a string after composition, no pruning required";
      } else {
        fst::Prune(&lattice, 0.1);
        if (!lattice.Properties(fst::kString, true)) {
          output << "ERROR_ambiguous_output: " << phrase;
          success = false;
          continue;
        }
      }
      fst::RmEpsilon(&lattice);
      output << festus::OneString(lattice, *output_label_maker);
    }
    (success ? std::cout : std::cerr) << output.str() << std::endl;
  }

  return 0;
}
