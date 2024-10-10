/*

Osmium -- OpenStreetMap data manipulation command line tool
https://osmcode.org/osmium-tool/

Copyright (C) 2013-2023  Jochen Topf <jochen@topf.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/


/*
- Question: Does our stack rely on the parent-discovery semantics of the smart strategy
  for boundary relations?
  I think not because the smart strategy does not collect ways of parent boundary relations,
  so they cannot be used to assign relation properties to ways inside the parent boundary.
  Also the assignment of ISO codes happens right at the beginning of region preprocessing.
 */

#include "strategy_smart_custom.hpp"

#include <unordered_set>

#include "../util.hpp"

#include <osmium/handler/check_order.hpp>
#include <osmium/tags/taglist.hpp>
#include <osmium/util/misc.hpp>
#include <osmium/util/string.hpp>

namespace strategy_smart_custom {

    void Data::add_relation_members(const osmium::Relation& relation) {
        for (const auto& member : relation.members()) {
            const auto ref = member.positive_ref();
            switch (member.type()) {
                case osmium::item_type::node:
                    if (!node_ids.get(ref)) {
                        extra_node_ids.set(ref);
                    }
                    break;
                case osmium::item_type::way:
                    if (!way_ids.get(ref)) {
                        extra_way_ids.set(ref);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    void Data::add_relation_network(const osmium::index::RelationsMapIndexes& indices) {
        std::queue<osmium::unsigned_object_id_type> q;
        const auto queue_relation = [&q](const auto id) {
            q.push(id);
        };
        const auto queue_relations = [&](const auto id) {
            indices.member_to_parent().for_each_parent(id, queue_relation);
            indices.parent_to_member().for_each_parent(id, queue_relation);
        };
        for (const auto id : relation_ids) {
            queue_relations(id);
        }
        while (!q.empty()) {
            const auto id = q.front();
            q.pop();
            if (!relation_ids.get(id) && !extra_relation_ids.get(id)) {
                extra_relation_ids.set(id);
                queue_relations(id);
            }
        }
    }

    Strategy::Strategy(const std::vector<std::unique_ptr<Extract>>& extracts, const osmium::Options& options) {

        m_extracts.reserve(extracts.size());
        for (const auto& extract : extracts) {
            m_extracts.emplace_back(*extract);
        }

        for (const auto& option : options) {
            if (option.first == "relations") {
                m_relation_tags.insert(m_relation_tags.begin(), option.second.begin(), option.second.end());
                add_filter_rules(m_relation_filter, option.second);
            } else if (option.first == "relation-system") {
                m_relation_system_tags.insert(m_relation_system_tags.begin(), option.second.begin(), option.second.end());
                add_filter_rules(m_relation_system_filter, option.second);
            } else if (option.first == "by-first-node") {
                m_by_first_node = option.second.empty() || option.second == "true" || option.second == "yes";
            } else {
                warning(std::string{"Ignoring unknown option '"} + option.first + "' for 'smart_custom' strategy.\n");
            }
        }

    }

    void Strategy::add_filter_rules(osmium::TagsFilter& filter, const std::string& option_value) noexcept {
        filter.set_default_result(false);
        const auto tags = osmium::split_string(option_value, ',', true);
        for (const auto &tag : tags) {
            const auto pos = tag.find(':');
            if (pos == std::string::npos) {
                filter.add_rule(true, osmium::TagMatcher{tag});
            } else {
                const auto key = tag.substr(0, pos);
                const auto value = tag.substr(pos + 1);
                filter.add_rule(true, osmium::TagMatcher{key, value});
            }
        }
    }

    const char* Strategy::name() const noexcept {
        return "smart_custom";
    }

    bool Strategy::is_relevant_relation(const osmium::Relation& relation) const noexcept {
        return osmium::tags::match_any_of(relation.tags(), m_relation_filter);
    }

    bool Strategy::is_part_of_relevant_relation_system(const osmium::Relation& relation) const noexcept {
        return osmium::tags::match_any_of(relation.tags(), m_relation_system_filter);
    }

    void Strategy::show_arguments(osmium::VerboseOutput& vout) {
        vout << "Additional strategy options:\n";
        if (!m_relation_tags.empty()) {
            vout << "  - [relations] " << m_relation_tags << '\n';
        }
        if (!m_relation_system_tags.empty()) {
            vout << "  - [relation-systems] " << m_relation_system_tags << '\n';
        }
        if (m_by_first_node) {
            vout << "  - [by-first-node]\n";
        }
        vout << '\n';
    }

    class Pass1 : public Pass<Strategy, Pass1> {

        osmium::handler::CheckOrder m_check_order;
        osmium::index::RelationsMapStash m_relations_map_stash;

    public:

        explicit Pass1(Strategy* strategy) :
            Pass(strategy) {
        }

        void node(const osmium::Node& node) {
            m_check_order.node(node);
        }

        void enode(extract_data* e, const osmium::Node& node) {
            if (e->contains(node.location())) {
                e->node_ids.set(node.positive_id());
            }
        }

        void way(const osmium::Way& way) {
            m_check_order.way(way);
        }

        void eway(extract_data* e, const osmium::Way& way) {
            if (strategy().m_by_first_node) {
                const auto& first_node = way.nodes().front();
                if ((e->node_ids.get(first_node.positive_ref()) && !e->has_conflicting_tags(way.tags())) || e->has_matching_tags(way.tags())) {
                    e->way_ids.set(way.positive_id());
                    for (const auto& nr : way.nodes()) {
                        if (!e->node_ids.get(nr.positive_ref())) {
                            e->extra_node_ids.set(nr.positive_ref());
                        }
                    }
                }
            } else {
                for (const auto& nr : way.nodes()) {
                    if (e->node_ids.get(nr.positive_ref())) {
                        e->way_ids.set(way.positive_id());
                        for (const auto& nr : way.nodes()) {
                            if (!e->node_ids.get(nr.positive_ref())) {
                                e->extra_node_ids.set(nr.positive_ref());
                            }
                        }
                        return;
                    }
                }
            }
        }

        void relation(const osmium::Relation& relation) {
            m_check_order.relation(relation);
            if (strategy().is_part_of_relevant_relation_system(relation)) {
                m_relations_map_stash.add_members(relation);
            }
        }

        void erelation(extract_data* e, const osmium::Relation& relation) {
            for (const auto& member : relation.members()) {
                switch (member.type()) {
                    case osmium::item_type::node:
                        if (e->node_ids.get(member.positive_ref())) {
                            e->relation_ids.set(relation.positive_id());
                            if (strategy().is_relevant_relation(relation) || strategy().is_part_of_relevant_relation_system(relation)) {
                                e->add_relation_members(relation);
                            }
			    return;
                        }
                        break;
                    case osmium::item_type::way:
                        if (e->way_ids.get(member.positive_ref())) {
                            e->relation_ids.set(relation.positive_id());
                            if (strategy().is_relevant_relation(relation) || strategy().is_part_of_relevant_relation_system(relation)) {
                                e->add_relation_members(relation);
                            }
			    return;
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        osmium::index::RelationsMapStash& relations_map_stash() noexcept {
            return m_relations_map_stash;
        }

    }; // class Pass1

    class Pass2 : public Pass<Strategy, Pass2> {

    public:

        explicit Pass2(Strategy* strategy) :
            Pass(strategy) {
        }

        void erelation(extract_data* e, const osmium::Relation& relation) {
            if (e->extra_relation_ids.get(relation.positive_id()) &&
                (strategy().is_part_of_relevant_relation_system(relation))) {
                e->add_relation_members(relation);
            }
        }

    }; // class Pass2

    class Pass3 : public Pass<Strategy, Pass3> {

    public:

        explicit Pass3(Strategy* strategy) :
            Pass(strategy) {
        }

        void eway(extract_data* e, const osmium::Way& way) {
            if (e->extra_way_ids.get(way.positive_id())) {
                for (const auto& nr : way.nodes()) {
                    if (!e->node_ids.get(nr.positive_ref())) {
                        e->extra_node_ids.set(nr.positive_ref());
                    }
                }
            }
        }

    }; // class Pass3

    class Pass4 : public Pass<Strategy, Pass4> {

    public:

        explicit Pass4(Strategy* strategy) :
            Pass(strategy) {
        }

        void enode(extract_data* e, const osmium::Node& node) {
            if (e->node_ids.get(node.positive_id()) ||
                e->extra_node_ids.get(node.positive_id())) {
                e->write(node);
            }
        }

        void eway(extract_data* e, const osmium::Way& way) {
            if (e->way_ids.get(way.positive_id()) ||
                e->extra_way_ids.get(way.positive_id())) {
                e->write(way);
            }
        }

        void erelation(extract_data* e, const osmium::Relation& relation) {
            if (e->relation_ids.get(relation.positive_id()) ||
                e->extra_relation_ids.get(relation.positive_id())) {
                e->write(relation);
            }
        }

    }; // class Pass4

    void Strategy::run(osmium::VerboseOutput& vout, bool display_progress, const osmium::io::File& input_file) {
        if (input_file.filename().empty()) {
            throw osmium::io_error{"Can not read from STDIN when using 'smart_custom' strategy."};
        }

        vout << "Running 'smart_custom' strategy in (at most) four passes...\n";
        const std::size_t file_size = osmium::file_size(input_file.filename());
        osmium::ProgressBar progress_bar{file_size * 4, !vout.verbose() && display_progress};

        vout << "Pass 1...\n";
        Pass1 pass1{this};
        pass1.run(progress_bar, input_file, osmium::io::read_meta::no);
        progress_bar.file_done(file_size);
        vout << "Pass 1 done\n";

        // identify the relations to include
        const auto relation_indices = pass1.relations_map_stash().build_indexes();
        for (auto& e : m_extracts) {
            e.add_relation_network(relation_indices);
        }

        if (std::find_if(m_extracts.begin(), m_extracts.end(),
            [](const auto& extract) { return !extract.extra_relation_ids.empty(); }) != m_extracts.end())
        {
            progress_bar.remove();
            vout << "Pass 2...\n";
            Pass2 pass2{this};
            pass2.run(progress_bar, input_file, osmium::osm_entity_bits::relation, osmium::io::read_meta::no);
            progress_bar.file_done(file_size);
            vout << "Pass 2 done\n";
        }

        if (std::find_if(m_extracts.begin(), m_extracts.end(),
            [](const auto& extract) { return !extract.extra_way_ids.empty(); }) != m_extracts.end())
        {
            progress_bar.remove();
            vout << "Pass 3...\n";
            Pass3 pass3{this};
            pass3.run(progress_bar, input_file, osmium::osm_entity_bits::way, osmium::io::read_meta::no);
            progress_bar.file_done(file_size);
            vout << "Pass 3 done\n";
        }

        progress_bar.remove();
        vout << "Pass 4...\n";
        Pass4 pass4{this};
        pass4.run(progress_bar, input_file);
        progress_bar.done();
        vout << "Pass 4 done\n";

    }

} // namespace strategy_smart_custom

