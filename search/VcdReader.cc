// OpenSTA, Static Timing Analyzer
// Copyright (c) 2022, Parallax Software, Inc.
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include <cctype>

#include "VcdReader.hh"

#include "Zlib.hh"
#include "Report.hh"
#include "StringUtil.hh"
#include "StaState.hh"

namespace sta {

using std::isspace;

// Very imprecise syntax definition
// https://en.wikipedia.org/wiki/Value_change_dump#Structure.2FSyntax
// Much better syntax definition
// https://web.archive.org/web/20120323132708/http://www.beyondttl.com/vcd.php

class VcdReader : public StaState
{
public:
  VcdReader(StaState *sta);
  Vcd read(const char *filename);

private:
  void parseTimescale();
  void setTimeUnit(const string &time_unit);
  void parseVar();
  void parseScope();
  void parseUpscope();
  void parseVarValues();
  string getToken();
  string readStmtString();
  vector<string> readStmtTokens();

  gzFile stream_;
  string token_;
  const char *filename_;
  int file_line_;
  int stmt_line_;

  Vcd *vcd_;
  VarTime time_;
  VarTime prev_time_;
  VcdScope scope_;
};

Vcd
readVcdFile(const char *filename,
            StaState *sta)

{
  VcdReader reader(sta);
  return reader.read(filename);
}

Vcd
VcdReader::read(const char *filename)
{
  Vcd vcd(this);
  vcd_ = &vcd;
  stream_ = gzopen(filename, "r");
  if (stream_) {
    filename_ = filename;
    file_line_ = 1;
    stmt_line_ = 1;
    string token = getToken();
    while (!token.empty()) {
      if (token == "$date")
        vcd_->setDate(readStmtString());
      else if (token == "$comment")
        vcd_->setComment(readStmtString());
      else if (token == "$version")
        vcd_->setVersion(readStmtString());
      else if (token == "$timescale")
        parseTimescale();
      else if (token == "$var")
        parseVar();
      else if (token == "$scope")
        parseScope();
      else if (token == "$upscope")
        parseUpscope();
      else if (token == "$enddefinitions")
        // empty body
        readStmtString();
      else if (token == "$dumpall")
        parseVarValues();
      else if (token == "$dumpvars")
        // Initial values.
        parseVarValues();
      else if (token[0] == '$')
        report_->fileError(800, filename_, stmt_line_, "unhandled vcd command.");
      else
        parseVarValues();
      token = getToken();
    }
    gzclose(stream_);
  }
  return vcd;
}

VcdReader::VcdReader(StaState *sta) :
  StaState(sta),
  stmt_line_(0),
  vcd_(nullptr),
  time_(0),
  prev_time_(0)
{
}

void
VcdReader::parseTimescale()
{
  vector<string> tokens = readStmtTokens();
  if (tokens.size() == 1) {
    size_t last;
    vcd_->setTimeScale(std::stod(tokens[0], &last));
    setTimeUnit(tokens[0].substr(last));
  }
  else if (tokens.size() == 2) {
    vcd_->setTimeScale(std::stod(tokens[0]));
    setTimeUnit(tokens[1]);
  }
  else
    report_->fileError(800, filename_, stmt_line_, "timescale syntax error.");
}

void
VcdReader::setTimeUnit(const string &time_unit)
{
  double time_unit_scale = 1.0;
  if (time_unit == "fs")
    time_unit_scale = 1e-15;
  else if (time_unit == "ps")
    time_unit_scale = 1e-12;
  else if (time_unit == "ns")
    time_unit_scale = 1e-9;
  else
    report_->fileError(801, filename_, stmt_line_, "Unknown timescale unit.");
  vcd_->setTimeUnit(time_unit, time_unit_scale);;
}

void
VcdReader::parseVar()
{
  vector<string> tokens = readStmtTokens();
  if (tokens.size() == 4
      || tokens.size() == 5) {
    string type_name = tokens[0];
    VcdVarType type = VcdVarType::wire;
    if (type_name == "wire")
      type = VcdVarType::wire;
    else if (type_name == "reg")
      type = VcdVarType::reg;
    else if (type_name == "parameter")
      type = VcdVarType::parameter;
    else
      report_->fileError(803, filename_, stmt_line_,
                         "Unknown variable type %s.",
                         type_name.c_str());

    int width = stoi(tokens[1]);
    string id = tokens[2];
    string name;

    int level = 0;
    for (string &context : scope_) {
      // Skip the first 2 levels of scope.
      //  -test bench module
      //  -design instance
      if (level > 1) {
        name += context;
        name += '/';
      }
      level++;
    }
    name += tokens[3];
    // iverilog separates bus base name from bit range.
    if (tokens.size() == 5)
      name += tokens[4];

    vcd_->makeVar(name, type, width, id);
  }
  else
    report_->fileError(802, filename_, stmt_line_, "Variable syntax error.");
}

void
VcdReader::parseScope()
{
  vector<string> tokens = readStmtTokens();
  string &scope = tokens[1];
  scope_.push_back(scope);
}

void
VcdReader::parseUpscope()
{
  readStmtTokens();
  scope_.pop_back();
}

void
VcdReader::parseVarValues()
{
  string token = getToken();
  while (!token.empty()) {
    if (token[0] == '#') {
      prev_time_ = time_;
      time_ = stoll(token.substr(1));
      if (time_ > prev_time_)
        vcd_->setMinDeltaTime(min(time_ - prev_time_, vcd_->minDeltaTime()));
    }
    else if (token[0] == '0'
             || token[0] == '1'
             || token[0] == 'X'
             || token[0] == 'U'
             || token[0] == 'Z') {
      string id = token.substr(1);
      if (!vcd_->varIdValid(id))
        report_->fileError(804, filename_, stmt_line_,
                           "unknown variable %s", id.c_str());
      vcd_->varAppendValue(id, time_, token[0]);
    }
    else if (token[0] == 'b') {
      if (token[1] == 'X'
          || token[1] == 'U'
          || token[1] == 'Z') {
        string id = getToken();
        if (!vcd_->varIdValid(id))
          report_->fileError(804, filename_, stmt_line_,
                             "unknown variable %s", id.c_str());
        // Bus mixed 0/1/X/U not supported.
        vcd_->varAppendValue(id, time_, token[1]);
      }
      else {
        string bin = token.substr(1);
        char *end;
        int64_t bus_value = strtol(bin.c_str(), &end, 2);
        string id = getToken();
        if (!vcd_->varIdValid(id))
          report_->fileError(804, filename_, stmt_line_,
                             "unknown variable %s", id.c_str());
        else
          vcd_->varAppendBusValue(id, time_, bus_value);
      }
    }
    token = getToken();
  }
  vcd_->setTimeMax(time_);
}

string
VcdReader::readStmtString()
{
  stmt_line_ = file_line_;
  string line;
  string token = getToken();
  while (!token.empty() && token != "$end") {
    if (!line.empty())
      line += " ";
    line += token;
    token = getToken();
  }
  return line;
}

vector<string>
VcdReader::readStmtTokens()
{
  stmt_line_ = file_line_;
  vector<string> tokens;
  string token = getToken();
  while (!token.empty() && token != "$end") {
    tokens.push_back(token);
    token = getToken();
  }
  return tokens;
}

string
VcdReader::getToken()
{
  string token;
  int ch = gzgetc(stream_);
  if (ch == '\n')
    file_line_++;
  // skip whitespace
  while (ch != EOF && isspace(ch)) {
    ch = gzgetc(stream_);
    if (ch == '\n')
      file_line_++;
  }
  while (ch != EOF && !isspace(ch)) {
    token.push_back(ch);
    ch = gzgetc(stream_);
    if (ch == '\n')
      file_line_++;
  }
  if (ch == EOF)
    return "";
  else
    return token;
}

////////////////////////////////////////////////////////////////

static void
reportWaveforms(Vcd &vcd,
                Report *report);

void
reportVcdWaveforms(const char *filename,
                   StaState *sta)

{
  VcdReader reader(sta);
  Vcd vcd = reader.read(filename);
  reportWaveforms(vcd, sta->report());
}

static void
reportWaveforms(Vcd &vcd,
                Report *report)
{
  report->reportLine("Date: %s", vcd.date().c_str());
  report->reportLine("Timescale: %.2f%s", vcd.timeScale(), vcd.timeUnit().c_str());
  // Characters per time sample.
  int zoom = (vcd.maxVarWidth() + 7) / 4;
  int time_delta = vcd.minDeltaTime();

  int max_var_name_length = vcd.maxVarNameLength();
  for (VcdVar &var : vcd.vars()) {
    string line;
    stringPrint(line, " %-*s",
                static_cast<int>(max_var_name_length),
                var.name().c_str());
    const VcdValues &var_values = vcd.values(var);
    if (!var_values.empty()) {
      size_t value_index = 0;
      VcdValue var_value = var_values[value_index];
      VcdValue prev_var_value = var_values[value_index];
      VarTime next_value_time = var_values[value_index + 1].time();
      for (double time = 0.0; time < vcd.timeMax(); time += time_delta) {
        if (time >= next_value_time) {
          if (value_index < var_values.size() - 1)
            value_index++;
          var_value = var_values[value_index];
          if (value_index < var_values.size())
            next_value_time = var_values[value_index + 1].time();
        }
        if (var_value.value()) {
          // 01UZX
          char value = var_value.value();
          char prev_value = prev_var_value.value();
          if (var.width() == 1) {
            if (value == '0' || value == '1') {
              for (int z = 0; z < zoom; z++) {
                if (z == 0
                    && value != prev_value
                    && (prev_value == '0'
                        || prev_value == '1'))
                  line += (prev_value == '1') ? "╲" : "╱";
                else
                  line += (value == '1') ? "▔" : "▁";
              }
            }
            else {
              string field;
              stringPrint(field, "%-*c", zoom, value);
              line += field;
            }
          }
          else {
            string field;
            stringPrint(field, "%-*c", zoom, value);
            line += field;
          }
        }
        else {
          // bus
          string field;
          stringPrint(field, "%-*llX", zoom, var_value.busValue());
          line += field;
        }
        prev_var_value = var_value;
      }
    }
    report->reportLineString(line);
  }
}

}
