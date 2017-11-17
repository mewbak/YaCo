//  Copyright (C) 2017 The YaCo Authors
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "Ida.h"
#include "Hooks.hpp"

#include "Repository.hpp"
#include "YaToolsHashProvider.hpp"
#include "IModel.hpp"
#include "Model.hpp"
#include "IDANativeModel.hpp"
#include "XML/XMLExporter.hpp"
#include "Logger.h"
#include "Yatools.h"
#include "Utils.hpp"

#define MODULE_NAME "hooks"
#include "IDAUtils.hpp"

#include <memory>
#include <tuple>
#include <chrono>

#ifdef _MSC_VER
#   include <filesystem>
#else
#   include <experimental/filesystem>
#endif

namespace fs = std::experimental::filesystem;

namespace
{
    std::string get_cache_folder_path()
    {
        std::string cache_folder_path = get_path(PATH_TYPE_IDB);
        remove_substring(cache_folder_path, fs::path(cache_folder_path).filename().string());
        cache_folder_path += "cache";
        return cache_folder_path;
    }

    struct Hooks
        : public IHooks
    {

        Hooks(const std::shared_ptr<IHashProvider>& hash_provider, const std::shared_ptr<IRepository>& repo_manager);

        void rename(ea_t ea, const std::string& new_name, const std::string& type, const std::string& old_name) override;
        void change_comment(ea_t ea) override;
        void undefine(ea_t ea) override;
        void delete_function(ea_t ea) override;
        void make_code(ea_t ea) override;
        void make_data(ea_t ea) override;
        void add_function(ea_t ea) override;
        void update_structure(ea_t struct_id) override;
        void update_structure_member(tid_t struct_id, tid_t member_id, ea_t member_offset) override;
        void delete_structure_member(tid_t struct_id, tid_t member_id, ea_t offset) override;
        void update_enum(enum_t enum_id) override;
        void change_operand_type(ea_t ea) override;
        void add_segment(ea_t start_ea, ea_t end_ea) override;
        void change_type_information(ea_t ea) override;

        void save() override;

        void flush() override;

    private:
        void add_address_to_process(ea_t ea, const std::string& message);
        void add_strucmember_to_process(ea_t struct_id, tid_t member_id, ea_t member_offset, const std::string& message);

        void save_structures(std::shared_ptr<IModelIncremental>& ida_model, IModelVisitor* memory_exporter);
        void save_enums(std::shared_ptr<IModelIncremental>& ida_model, IModelVisitor* memory_exporter);

        std::shared_ptr<IHashProvider> hash_provider_;
        std::shared_ptr<IRepository> repo_manager_;

        std::set<ea_t> addresses_to_process_;
        std::set<tid_t> structures_to_process_;
        std::map<tid_t, std::tuple<tid_t, ea_t>> structmember_to_process_; // map<struct_id, tuple<member_id, offset>>
        std::set<enum_t> enums_to_process_;
        std::map<ea_t, tid_t> enummember_to_process_;
        std::set<ea_t> comments_to_process_;
        std::set<std::tuple<ea_t, ea_t>> segments_to_process_; // set<tuple<seg_ea_start, seg_ea_end>>
    };
}

Hooks::Hooks(const std::shared_ptr<IHashProvider>& hash_provider, const std::shared_ptr<IRepository>& repo_manager)
    : hash_provider_{ hash_provider }
    , repo_manager_{ repo_manager }
{

}

void Hooks::rename(ea_t ea, const std::string& new_name, const std::string& type, const std::string& old_name)
{
    std::string message{ type };
    if (!type.empty())
        message += ' ';
    message += "renamed ";
    if (!old_name.empty())
    {
        message += "from ";
        message += old_name;
    }
    message += "to ";
    message += new_name;
    add_address_to_process(ea, message);
}

void Hooks::change_comment(ea_t ea)
{
    comments_to_process_.insert(ea);
}

void Hooks::undefine(ea_t ea)
{
    add_address_to_process(ea, "Undefne");
}

void Hooks::delete_function(ea_t ea)
{
    add_address_to_process(ea, "Delete function");
}

void Hooks::make_code(ea_t ea)
{
    add_address_to_process(ea, "Create code");
}

void Hooks::make_data(ea_t ea)
{
    add_address_to_process(ea, "Create data");
}

void Hooks::add_function(ea_t ea)
{
    // Comments from Python:
    // invalid all addresses in this function(they depend(relatively) on this function now, no on code)
    // Warning : deletion of objects not implemented
    // TODO : implement deletion of objects inside newly created function range
    // TODO : use function chunks to iterate over function code
    add_address_to_process(ea, "Create function");
}

void Hooks::update_structure(ea_t struct_id)
{
    structures_to_process_.insert(struct_id);
    repo_manager_->add_auto_comment(struct_id, "Updated");
}

void Hooks::update_structure_member(tid_t struct_id, tid_t member_id, ea_t member_offset)
{
    std::string message{ "Member updated at offset " };
    message += ea_to_hex(member_offset);
    message += " : ";
    qstring member_id_fullname;
    get_member_fullname(&member_id_fullname, member_id);
    message += member_id_fullname.c_str();
    add_strucmember_to_process(struct_id, member_id, member_offset, message);
}

void Hooks::delete_structure_member(tid_t struct_id, tid_t member_id, ea_t offset)
{
    add_strucmember_to_process(struct_id, member_id, offset, "Member deleted");
}

void Hooks::update_enum(enum_t enum_id)
{
    enums_to_process_.insert(enum_id);
    repo_manager_->add_auto_comment(enum_id, "Updated");
}

void Hooks::change_operand_type(ea_t ea)
{
    if (get_func(ea) || is_code(get_flags(ea)))
    {
        addresses_to_process_.insert(ea);
        repo_manager_->add_auto_comment(ea, "Operand type change");
        return;
    }

    if (is_member_id(ea))
        return; // this is a member id: hook already present (update_structure_member)

    IDA_LOG_WARNING("Operand type changed at %s, code out of a function: not implemented", ea_to_hex(ea).c_str());
}

void Hooks::add_segment(ea_t start_ea, ea_t end_ea)
{
    segments_to_process_.insert(std::make_tuple(start_ea, end_ea));
}

void Hooks::change_type_information(ea_t ea)
{
    add_address_to_process(ea, "Type information changed");
}

void Hooks::save()
{
    const auto time_start = std::chrono::system_clock::now();

    std::shared_ptr<IModelIncremental> ida_model = MakeModelIncremental(hash_provider_.get());
    ModelAndVisitor db = MakeModel();

    db.visitor->visit_start();

    // add comments to adresses to process
    for (ea_t ea : comments_to_process_)
        add_address_to_process(ea, "Changed comment");

    // process structures
    save_structures(ida_model, db.visitor.get());

    // process enums
    save_enums(ida_model, db.visitor.get());

    // process addresses
    for (ea_t ea : addresses_to_process_)
        ida_model->accept_ea(*db.visitor, ea);

    // process segments
    for (const std::tuple<ea_t, ea_t>& segment_ea : segments_to_process_)
        ida_model->accept_segment(*db.visitor, std::get<0>(segment_ea));

    db.visitor->visit_end();

    db.model->accept(*MakeXmlExporter(get_cache_folder_path()));

    const auto time_end = std::chrono::system_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(time_end - time_start);
    IDA_LOG_INFO("Saved in %lld seconds", elapsed.count());
}

void Hooks::flush()
{
    addresses_to_process_.clear();
    structures_to_process_.clear();
    structmember_to_process_.clear();
    enums_to_process_.clear();
    enummember_to_process_.clear();
    comments_to_process_.clear();
    segments_to_process_.clear();
}

void Hooks::add_address_to_process(ea_t ea, const std::string& message)
{
    addresses_to_process_.insert(ea);
    repo_manager_->add_auto_comment(ea, message);
}

void Hooks::add_strucmember_to_process(ea_t struct_id, tid_t member_id, ea_t member_offset, const std::string& message)
{
    structmember_to_process_[struct_id] = std::make_tuple(member_id, member_offset);
    repo_manager_->add_auto_comment(struct_id, message);
}

void Hooks::save_structures(std::shared_ptr<IModelIncremental>& ida_model, IModelVisitor* memory_exporter)
{
    // structures: export modified ones, delete deleted ones
    for (tid_t struct_id : structures_to_process_)
    {
        uval_t struct_idx = get_struc_idx(struct_id);
        if (struct_idx != BADADDR)
        {
            // structure or stackframe modified
            ida_model->accept_struct(*memory_exporter, BADADDR, struct_id);
            continue;
        }

        // structure or stackframe deleted
        // need to export the parent (function)
        ea_t func_ea = get_func_by_frame(struct_id);
        if (func_ea != BADADDR)
        {
            // if stackframe
            ida_model->accept_struct(*memory_exporter, func_ea, struct_id);
            ida_model->accept_ea(*memory_exporter, func_ea);
            continue;
        }
        // if structure
        ida_model->delete_struct(*memory_exporter, struct_id);
    }

    // structures members : update modified ones, remove deleted ones
    for (const std::pair<const tid_t, std::tuple<tid_t, ea_t>>& struct_info : structmember_to_process_)
    {
        tid_t struct_id = struct_info.first;
        ea_t member_offset = std::get<1>(struct_info.second);

        struc_t* ida_struct = get_struc(struct_id);
        uval_t struct_idx = get_struc_idx(struct_id);

        ea_t stackframe_func_addr = BADADDR;

        if (!ida_struct || struct_idx == BADADDR)
        {
            // structure or stackframe deleted
            ea_t func_ea = get_func_by_frame(struct_id);
            if (func_ea == BADADDR)
            {
                // if structure
                ida_model->delete_struct_member(*memory_exporter, BADADDR, struct_id, member_offset);
                continue;
            }
            // if stackframe
            stackframe_func_addr = func_ea;
            ida_model->accept_function(*memory_exporter, stackframe_func_addr);
        }

        // structure or stackframe modified
        member_t* ida_member = get_member(ida_struct, member_offset);
        if (!ida_member || ida_member->id == -1)
        {
            // if member deleted
            ida_model->delete_struct_member(*memory_exporter, stackframe_func_addr, struct_id, member_offset);
            continue;
        }

        if (member_offset > 0)
        {
            member_t* ida_prev_member = get_member(ida_struct, member_offset - 1);
            if (ida_prev_member && ida_prev_member->id == ida_member->id)
            {
                // if member deleted and replaced by member starting above it
                ida_model->delete_struct_member(*memory_exporter, stackframe_func_addr, struct_id, member_offset);
                continue;
            }
        }

        // if member updated
        ida_model->accept_struct_member(*memory_exporter, stackframe_func_addr, ida_member->id);
    }
}

void Hooks::save_enums(std::shared_ptr<IModelIncremental>& ida_model, IModelVisitor* memory_exporter)
{
    // enums: export modified ones, delete deleted ones
    for (enum_t enum_id : enums_to_process_)
    {
        uval_t enum_idx = get_enum_idx(enum_id);
        if (enum_idx == BADADDR)
        {
            // enum deleted
            ida_model->delete_enum(*memory_exporter, enum_id);
            continue;
        }

        // enum modified
        ida_model->accept_enum(*memory_exporter, enum_id);
    }

    // not implemented in Python, TODO after porting to C++ events
    // enums members : update modified ones, remove deleted ones
    /*
    iterate over members :
        -if the parent enum has been deleted, delete the member
        -otherwise, detect if the member has been updated or removed
            -updated : accept enum_member
            -removed : accept enum_member_deleted
    */
}


std::shared_ptr<IHooks> MakeHooks(const std::shared_ptr<IHashProvider>& hash_provider, const std::shared_ptr<IRepository>& repo_manager)
{
    return std::make_shared<Hooks>(hash_provider, repo_manager);
}
