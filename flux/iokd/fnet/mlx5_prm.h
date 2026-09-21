/*
 * Copyright (c) 2019 Mellanox Technologies, Inc. All rights reserved.
 *
 * This software is available under either the GNU General Public License
 * version 2 or the OpenIB.org BSD license. See the rdma-core COPYING file.
 *
 * This is the narrow PRM subset consumed by the Flux mlx5 external control
 * plane. Layouts are copied from the in-tree Junction/Caladan mlx5_ifc.h;
 * rdma-core remains the sole command transport and object owner.
 */
#ifndef FLUX_IOKD_FNET_MLX5_PRM_H
#define FLUX_IOKD_FNET_MLX5_PRM_H

#include <stdint.h>

#define u8 uint8_t

enum {
	MLX5_CMD_OP_CREATE_CQ = 0x400,
	MLX5_CMD_OP_QUERY_NIC_VPORT_CONTEXT = 0x754,
	MLX5_CMD_OP_MODIFY_NIC_VPORT_CONTEXT = 0x755,
	MLX5_CMD_OP_ALLOC_UAR = 0x802,
	MLX5_CMD_OP_DEALLOC_UAR = 0x803,
	MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN = 0x816,
	MLX5_CMD_OP_CREATE_TIR = 0x900,
	MLX5_CMD_OP_CREATE_SQ = 0x904,
	MLX5_CMD_OP_MODIFY_SQ = 0x905,
	MLX5_CMD_OP_CREATE_RQ = 0x908,
	MLX5_CMD_OP_MODIFY_RQ = 0x909,
	MLX5_CMD_OP_CREATE_TIS = 0x912,
	MLX5_CMD_OP_CREATE_RQT = 0x916,
	MLX5_CMD_OP_CREATE_FLOW_TABLE = 0x930,
	MLX5_CMD_OP_CREATE_FLOW_GROUP = 0x933,
	MLX5_CMD_OP_SET_FLOW_TABLE_ENTRY = 0x936,
};

enum {
	MLX5_REG_PMTU = 0x5003,
	DR_MATCHER_CRITERIA_OUTER = 1 << 0,
	MLX5_FLOW_DEST_TYPE_TIR = 0x2,
	MLX5_FLOW_CONTEXT_ACTION_FWD_DEST = 0x4,
};

struct mlx5_ifc_flow_table_context_bits {
	u8 reformat_en[0x1];
	u8 decap_en[0x1];
	u8 sw_owner[0x1];
	u8 termination_table[0x1];
	u8 table_miss_action[0x4];
	u8 level[0x8];
	u8 reserved_at_10[0x8];
	u8 log_size[0x8];

	u8 reserved_at_20[0x8];
	u8 table_miss_id[0x18];

	u8 reserved_at_40[0x8];
	u8 lag_master_next_table_id[0x18];

	u8 reserved_at_60[0x60];

	u8 sw_owner_icm_root_1[0x40];

	u8 sw_owner_icm_root_0[0x40];
};

struct mlx5_ifc_create_flow_table_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 other_vport[0x1];
	u8 reserved_at_41[0xf];
	u8 vport_number[0x10];

	u8 reserved_at_60[0x20];

	u8 table_type[0x8];
	u8 reserved_at_88[0x18];

	u8 reserved_at_a0[0x20];

	struct mlx5_ifc_flow_table_context_bits flow_table_context;
};

struct mlx5_ifc_create_flow_table_out_bits {
	u8 status[0x8];
	u8 icm_address_63_40[0x18];

	u8 syndrome[0x20];

	u8 icm_address_39_32[0x8];
	u8 table_id[0x18];

	u8 icm_address_31_0[0x20];
};

struct mlx5_ifc_pmtu_reg_bits {
	u8 reserved_at_0[0x8];
	u8 local_port[0x8];
	u8 reserved_at_10[0x10];

	u8 max_mtu[0x10];
	u8 reserved_at_30[0x10];

	u8 admin_mtu[0x10];
	u8 reserved_at_50[0x10];

	u8 oper_mtu[0x10];
	u8 reserved_at_70[0x10];
};

struct mlx5_ifc_mac_address_layout_bits {
	u8 reserved_at_0[0x10];
	u8 mac_addr_47_32[0x10];

	u8 mac_addr_31_0[0x20];
};

struct mlx5_ifc_nic_vport_context_bits {
	u8 reserved_at_0[0x5];
	u8 min_wqe_inline_mode[0x3];
	u8 reserved_at_8[0x15];
	u8 disable_mc_local_lb[0x1];
	u8 disable_uc_local_lb[0x1];
	u8 roce_en[0x1];

	u8 arm_change_event[0x1];
	u8 reserved_at_21[0x1a];
	u8 event_on_mtu[0x1];
	u8 event_on_promisc_change[0x1];
	u8 event_on_vlan_change[0x1];
	u8 event_on_mc_address_change[0x1];
	u8 event_on_uc_address_change[0x1];

	u8 reserved_at_40[0xc];

	u8 affiliation_criteria[0x4];
	u8 affiliated_vhca_id[0x10];

	u8 reserved_at_60[0xd0];

	u8 mtu[0x10];

	u8 system_image_guid[0x40];
	u8 port_guid[0x40];
	u8 node_guid[0x40];

	u8 reserved_at_200[0x140];
	u8 qkey_violation_counter[0x10];
	u8 reserved_at_350[0x430];

	u8 promisc_uc[0x1];
	u8 promisc_mc[0x1];
	u8 promisc_all[0x1];
	u8 reserved_at_783[0x2];
	u8 allowed_list_type[0x3];
	u8 reserved_at_788[0xc];
	u8 allowed_list_size[0xc];

	struct mlx5_ifc_mac_address_layout_bits permanent_address;

	u8 reserved_at_7e0[0x20];

	u8 current_uc_mac_address[][0x40];
};

struct mlx5_ifc_query_nic_vport_context_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x40];

	struct mlx5_ifc_nic_vport_context_bits nic_vport_context;
};

struct mlx5_ifc_query_nic_vport_context_in_bits {
	u8 opcode[0x10];
	u8 reserved_at_10[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 other_vport[0x1];
	u8 reserved_at_41[0xf];
	u8 vport_number[0x10];

	u8 reserved_at_60[0x5];
	u8 allowed_list_type[0x3];
	u8 reserved_at_68[0x18];
};

struct mlx5_ifc_modify_nic_vport_context_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_modify_nic_vport_field_select_bits {
	u8 reserved_at_0[0x12];
	u8 affiliation[0x1];
	u8 reserved_at_13[0x1];
	u8 disable_uc_local_lb[0x1];
	u8 disable_mc_local_lb[0x1];
	u8 node_guid[0x1];
	u8 port_guid[0x1];
	u8 min_inline[0x1];
	u8 mtu[0x1];
	u8 change_event[0x1];
	u8 promisc[0x1];
	u8 permanent_address[0x1];
	u8 addresses_list[0x1];
	u8 roce_en[0x1];
	u8 reserved_at_1f[0x1];
};

struct mlx5_ifc_modify_nic_vport_context_in_bits {
	u8 opcode[0x10];
	u8 reserved_at_10[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 other_vport[0x1];
	u8 reserved_at_41[0xf];
	u8 vport_number[0x10];

	struct mlx5_ifc_modify_nic_vport_field_select_bits field_select;

	u8 reserved_at_80[0x780];

	struct mlx5_ifc_nic_vport_context_bits nic_vport_context;
};

struct mlx5_ifc_create_tir_out_bits {
	u8 status[0x8];
	u8 icm_address_63_40[0x18];

	u8 syndrome[0x20];

	u8 icm_address_39_32[0x8];
	u8 tirn[0x18];

	u8 icm_address_31_0[0x20];
};

struct mlx5_ifc_tisc_bits {
	u8 strict_lag_tx_port_affinity[0x1];
	u8 tls_en[0x1];
	u8 reserved_at_2[0x2];
	u8 lag_tx_port_affinity[0x04];

	u8 reserved_at_8[0x4];
	u8 prio[0x4];
	u8 reserved_at_10[0x10];

	u8 reserved_at_20[0x100];

	u8 reserved_at_120[0x8];
	u8 transport_domain[0x18];

	u8 reserved_at_140[0x8];
	u8 underlay_qpn[0x18];

	u8 reserved_at_160[0x8];
	u8 pd[0x18];

	u8 reserved_at_180[0x380];
};

struct mlx5_ifc_ipv4_layout_bits {
	u8 reserved_at_0[0x60];

	u8 ipv4[0x20];
};

struct mlx5_ifc_ipv6_layout_bits {
	u8 ipv6[16][0x8];
};

union mlx5_ifc_ipv6_layout_ipv4_layout_auto_bits {
	struct mlx5_ifc_ipv6_layout_bits ipv6_layout;
	struct mlx5_ifc_ipv4_layout_bits ipv4_layout;
	u8 reserved_at_0[0x80];
};

struct mlx5_ifc_fte_match_set_lyr_2_4_bits {
	u8 smac_47_16[0x20];

	u8 smac_15_0[0x10];
	u8 ethertype[0x10];

	u8 dmac_47_16[0x20];

	u8 dmac_15_0[0x10];
	u8 first_prio[0x3];
	u8 first_cfi[0x1];
	u8 first_vid[0xc];

	u8 ip_protocol[0x8];
	u8 ip_dscp[0x6];
	u8 ip_ecn[0x2];
	u8 cvlan_tag[0x1];
	u8 svlan_tag[0x1];
	u8 frag[0x1];
	u8 ip_version[0x4];
	u8 tcp_flags[0x9];

	u8 tcp_sport[0x10];
	u8 tcp_dport[0x10];

	u8 reserved_at_c0[0x18];
	u8 ttl_hoplimit[0x8];

	u8 udp_sport[0x10];
	u8 udp_dport[0x10];

	union mlx5_ifc_ipv6_layout_ipv4_layout_auto_bits src_ipv4_src_ipv6;

	union mlx5_ifc_ipv6_layout_ipv4_layout_auto_bits dst_ipv4_dst_ipv6;
};

struct mlx5_ifc_nvgre_key_bits {
	u8 hi[0x18];
	u8 lo[0x8];
};

union mlx5_ifc_gre_key_bits {
	struct mlx5_ifc_nvgre_key_bits nvgre;
	u8 key[0x20];
};

struct mlx5_ifc_fte_match_set_misc_bits {
	u8 gre_c_present[0x1];
	u8 reserved_at_1[0x1];
	u8 gre_k_present[0x1];
	u8 gre_s_present[0x1];
	u8 source_vhca_port[0x4];
	u8 source_sqn[0x18];

	u8 source_eswitch_owner_vhca_id[0x10];
	u8 source_port[0x10];

	u8 outer_second_prio[0x3];
	u8 outer_second_cfi[0x1];
	u8 outer_second_vid[0xc];
	u8 inner_second_prio[0x3];
	u8 inner_second_cfi[0x1];
	u8 inner_second_vid[0xc];

	u8 outer_second_cvlan_tag[0x1];
	u8 inner_second_cvlan_tag[0x1];
	u8 outer_second_svlan_tag[0x1];
	u8 inner_second_svlan_tag[0x1];
	u8 reserved_at_64[0xc];
	u8 gre_protocol[0x10];

	union mlx5_ifc_gre_key_bits gre_key;

	u8 vxlan_vni[0x18];
	u8 bth_opcode[0x8];

	u8 geneve_vni[0x18];
	u8 reserved_at_d8[0x6];
	u8 geneve_tlv_option_0_exist[0x1];
	u8 geneve_oam[0x1];

	u8 reserved_at_e0[0xc];
	u8 outer_ipv6_flow_label[0x14];

	u8 reserved_at_100[0xc];
	u8 inner_ipv6_flow_label[0x14];

	u8 reserved_at_120[0xa];
	u8 geneve_opt_len[0x6];
	u8 geneve_protocol_type[0x10];

	u8 reserved_at_140[0x8];
	u8 bth_dst_qp[0x18];
	u8 reserved_at_160[0x20];
	u8 outer_esp_spi[0x20];
	u8 reserved_at_1a0[0x60];
};

struct mlx5_ifc_fte_match_mpls_bits {
	u8 mpls_label[0x14];
	u8 mpls_exp[0x3];
	u8 mpls_s_bos[0x1];
	u8 mpls_ttl[0x8];
};

struct mlx5_ifc_fte_match_set_misc2_bits {
	struct mlx5_ifc_fte_match_mpls_bits outer_first_mpls;

	struct mlx5_ifc_fte_match_mpls_bits inner_first_mpls;

	struct mlx5_ifc_fte_match_mpls_bits outer_first_mpls_over_gre;

	struct mlx5_ifc_fte_match_mpls_bits outer_first_mpls_over_udp;

	u8 metadata_reg_c_7[0x20];

	u8 metadata_reg_c_6[0x20];

	u8 metadata_reg_c_5[0x20];

	u8 metadata_reg_c_4[0x20];

	u8 metadata_reg_c_3[0x20];

	u8 metadata_reg_c_2[0x20];

	u8 metadata_reg_c_1[0x20];

	u8 metadata_reg_c_0[0x20];

	u8 metadata_reg_a[0x20];

	u8 reserved_at_1a0[0x60];
};

struct mlx5_ifc_fte_match_set_misc3_bits {
	u8 inner_tcp_seq_num[0x20];

	u8 outer_tcp_seq_num[0x20];

	u8 inner_tcp_ack_num[0x20];

	u8 outer_tcp_ack_num[0x20];

	u8 reserved_at_80[0x8];
	u8 outer_vxlan_gpe_vni[0x18];

	u8 outer_vxlan_gpe_next_protocol[0x8];
	u8 outer_vxlan_gpe_flags[0x8];
	u8 reserved_at_b0[0x10];

	u8 icmp_header_data[0x20];

	u8 icmpv6_header_data[0x20];

	u8 icmp_type[0x8];
	u8 icmp_code[0x8];
	u8 icmpv6_type[0x8];
	u8 icmpv6_code[0x8];

	u8 geneve_tlv_option_0_data[0x20];

	u8 gtpu_teid[0x20];

	u8 gtpu_msg_type[0x8];
	u8 gtpu_msg_flags[0x8];
	u8 reserved_at_170[0x10];

	u8 gtpu_dw_2[0x20];

	u8 gtpu_first_ext_dw_0[0x20];

	u8 gtpu_dw_0[0x20];

	u8 reserved_at_1e0[0x20];
};

struct mlx5_ifc_fte_match_set_misc4_bits {
	u8 prog_sample_field_value_0[0x20];

	u8 prog_sample_field_id_0[0x20];

	u8 prog_sample_field_value_1[0x20];

	u8 prog_sample_field_id_1[0x20];

	u8 prog_sample_field_value_2[0x20];

	u8 prog_sample_field_id_2[0x20];

	u8 prog_sample_field_value_3[0x20];

	u8 prog_sample_field_id_3[0x20];

	u8 reserved_at_100[0x100];
};

struct mlx5_ifc_fte_match_set_misc5_bits {
	u8 macsec_tag_0[0x20];

	u8 macsec_tag_1[0x20];

	u8 macsec_tag_2[0x20];

	u8 macsec_tag_3[0x20];

	u8 tunnel_header_0[0x20];

	u8 tunnel_header_1[0x20];

	u8 tunnel_header_2[0x20];

	u8 tunnel_header_3[0x20];

	u8 reserved_at_100[0x100];
};

struct mlx5_ifc_fte_match_param_bits {
	struct mlx5_ifc_fte_match_set_lyr_2_4_bits outer_headers;

	struct mlx5_ifc_fte_match_set_misc_bits misc_parameters;

	struct mlx5_ifc_fte_match_set_lyr_2_4_bits inner_headers;

	struct mlx5_ifc_fte_match_set_misc2_bits misc_parameters_2;

	struct mlx5_ifc_fte_match_set_misc3_bits misc_parameters_3;

	struct mlx5_ifc_fte_match_set_misc4_bits misc_parameters_4;

	struct mlx5_ifc_fte_match_set_misc5_bits misc_parameters_5;

	u8 reserved_at_e00[0x200];
};

struct mlx5_ifc_create_flow_group_in_bits {
	u8 opcode[0x10];
	u8 reserved_at_10[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 other_vport[0x1];
	u8 reserved_at_41[0xf];
	u8 vport_number[0x10];

	u8 reserved_at_60[0x20];

	u8 table_type[0x8];
	u8 reserved_at_88[0x4];
	u8 group_type[0x4];
	u8 reserved_at_90[0x10];

	u8 reserved_at_a0[0x8];
	u8 table_id[0x18];

	u8 source_eswitch_owner_vhca_id_valid[0x1];

	u8 reserved_at_c1[0x1f];

	u8 start_flow_index[0x20];

	u8 reserved_at_100[0x20];

	u8 end_flow_index[0x20];

	u8 reserved_at_140[0x10];
	u8 match_definer_id[0x10];

	u8 reserved_at_160[0x80];

	u8 reserved_at_1e0[0x18];
	u8 match_criteria_enable[0x8];

	struct mlx5_ifc_fte_match_param_bits match_criteria;

	u8 reserved_at_1200[0xe00];
};

struct mlx5_ifc_create_flow_group_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x8];
	u8 group_id[0x18];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_dest_format_bits {
	u8 destination_type[0x8];
	u8 destination_id[0x18];

	u8 reserved_at_20[0x1];
	u8 packet_reformat[0x1];
	u8 reserved_at_22[0x1e];
};

struct mlx5_ifc_extended_dest_format_bits {
	struct mlx5_ifc_dest_format_bits destination_entry;

	u8 packet_reformat_id[0x20];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_flow_counter_list_bits {
	u8 flow_counter_id[0x20];

	u8 reserved_at_20[0x20];
};

struct mlx5_ifc_vlan_bits {
	u8 ethtype[0x10];
	u8 prio[0x3];
	u8 cfi[0x1];
	u8 vid[0xc];
};

union mlx5_ifc_dest_format_struct_flow_counter_list_auto_bits {
	struct mlx5_ifc_extended_dest_format_bits extended_dest_format;
	struct mlx5_ifc_flow_counter_list_bits flow_counter_list;
};

struct mlx5_ifc_flow_context_bits {
	struct mlx5_ifc_vlan_bits push_vlan;

	u8 group_id[0x20];

	u8 reserved_at_40[0x8];
	u8 flow_tag[0x18];

	u8 reserved_at_60[0x10];
	u8 action[0x10];

	u8 extended_destination[0x1];
	u8 reserved_at_81[0x1];
	u8 flow_source[0x2];
	u8 reserved_at_84[0x4];
	u8 destination_list_size[0x18];

	u8 reserved_at_a0[0x8];
	u8 flow_counter_list_size[0x18];

	u8 packet_reformat_id[0x20];

	u8 modify_header_id[0x20];

	struct mlx5_ifc_vlan_bits push_vlan_2;

	u8 ipsec_obj_id[0x20];
	u8 reserved_at_140[0xc0];

	struct mlx5_ifc_fte_match_param_bits match_value;

	u8 reserved_at_1200[0x600];

	union mlx5_ifc_dest_format_struct_flow_counter_list_auto_bits
		destination[];
};

struct mlx5_ifc_set_fte_in_bits {
	u8 opcode[0x10];
	u8 reserved_at_10[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 other_vport[0x1];
	u8 reserved_at_41[0xf];
	u8 vport_number[0x10];

	u8 reserved_at_60[0x20];

	u8 table_type[0x8];
	u8 reserved_at_88[0x18];

	u8 reserved_at_a0[0x8];
	u8 table_id[0x18];

	u8 reserved_at_c0[0x40];
	u8 flow_index[0x20];

	u8 reserved_at_120[0xe0];
	struct mlx5_ifc_flow_context_bits flow_context;
};

struct mlx5_ifc_set_fte_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_wq_bits {
	u8 wq_type[0x4];
	u8 wq_signature[0x1];
	u8 end_padding_mode[0x2];
	u8 cd_slave[0x1];
	u8 reserved_at_8[0x18];
	u8 hds_skip_first_sge[0x1];
	u8 log2_hds_buf_size[0x3];
	u8 reserved_at_24[0x7];
	u8 page_offset[0x5];
	u8 lwm[0x10];
	u8 reserved_at_40[0x8];
	u8 pd[0x18];
	u8 reserved_at_60[0x8];
	u8 uar_page[0x18];
	u8 dbr_addr[0x40];
	u8 hw_counter[0x20];
	u8 sw_counter[0x20];
	u8 reserved_at_100[0xc];
	u8 log_wq_stride[0x4];
	u8 reserved_at_110[0x3];
	u8 log_wq_pg_sz[0x5];
	u8 reserved_at_118[0x3];
	u8 log_wq_sz[0x5];
	u8 dbr_umem_valid[0x1];
	u8 wq_umem_valid[0x1];
	u8 reserved_at_122[0x1];
	u8 log_hairpin_num_packets[0x5];
	u8 reserved_at_128[0x3];
	u8 log_hairpin_data_sz[0x5];
	u8 reserved_at_130[0x4];
	u8 single_wqe_log_num_of_strides[0x4];
	u8 two_byte_shift_en[0x1];
	u8 reserved_at_139[0x4];
	u8 single_stride_log_num_of_bytes[0x3];
	u8 dbr_umem_id[0x20];
	u8 wq_umem_id[0x20];
	u8 wq_umem_offset[0x40];
	u8 reserved_at_1c0[0x440];
};

struct mlx5_ifc_sqc_bits {
	u8 rlky[0x1];
	u8 cd_master[0x1];
	u8 fre[0x1];
	u8 flush_in_error_en[0x1];
	u8 allow_multi_pkt_send_wqe[0x1];
	u8 min_wqe_inline_mode[0x3];
	u8 state[0x4];
	u8 reg_umr[0x1];
	u8 allow_swp[0x1];
	u8 hairpin[0x1];
	u8 reserved_at_f[0xb];
	u8 ts_format[0x2];
	u8 reserved_at_1c[0x4];

	u8 reserved_at_20[0x8];
	u8 user_index[0x18];

	u8 reserved_at_40[0x8];
	u8 cqn[0x18];

	u8 reserved_at_60[0x8];
	u8 hairpin_peer_rq[0x18];

	u8 reserved_at_80[0x10];
	u8 hairpin_peer_vhca[0x10];

	u8 reserved_at_a0[0x20];

	u8 reserved_at_c0[0x8];
	u8 ts_cqe_to_dest_cqn[0x18];

	u8 reserved_at_e0[0x10];
	u8 packet_pacing_rate_limit_index[0x10];
	u8 tis_lst_sz[0x10];
	u8 qos_queue_group_id[0x10];

	u8 reserved_at_120[0x40];

	u8 reserved_at_160[0x8];
	u8 tis_num_0[0x18];

	struct mlx5_ifc_wq_bits wq;
};

struct mlx5_ifc_modify_sq_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_create_sq_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x8];
	u8 sqn[0x18];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_modify_sq_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 sq_state[0x4];
	u8 reserved_at_44[0x4];
	u8 sqn[0x18];

	u8 reserved_at_60[0x20];

	u8 modify_bitmask[0x40];

	u8 reserved_at_c0[0x40];

	struct mlx5_ifc_sqc_bits sq_context;
};

struct mlx5_ifc_mbox_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_alloc_uar_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x8];
	u8 uar[0x18];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_alloc_uar_in_bits {
	u8 opcode[0x10];
	u8 reserved_at_10[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_dealloc_uar_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_dealloc_uar_in_bits {
	u8 opcode[0x10];
	u8 reserved_at_10[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 reserved_at_40[0x8];
	u8 uar[0x18];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_create_cq_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x8];
	u8 cqn[0x18];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_alloc_transport_domain_out_bits {
	u8 reserved_at_0[0x40];

	u8 reserved_at_40[0x8];
	u8 transport_domain[0x18];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_create_rq_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];
	u8 syndrome[0x20];
	u8 reserved_at_40[0x8];
	u8 rqn[0x18];
	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_create_rqt_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x8];
	u8 rqtn[0x18];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_create_tis_out_bits {
	u8 reserved_at_0[0x40];

	u8 reserved_at_40[0x8];
	u8 tisn[0x18];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_rx_hash_field_select_bits {
	u8 l3_prot_type[0x1];
	u8 l4_prot_type[0x1];
	u8 selected_fields[0x1e];
};

struct mlx5_ifc_tirc_bits {
	u8 reserved_at_0[0x20];
	u8 disp_type[0x4];
	u8 reserved_at_24[0x1c];
	u8 reserved_at_40[0x40];
	u8 reserved_at_80[0x4];
	u8 lro_timeout_period_usecs[0x10];
	u8 lro_enable_mask[0x4];
	u8 lro_max_msg_sz[0x8];
	u8 reserved_at_a0[0x40];
	u8 reserved_at_e0[0x8];
	u8 inline_rqn[0x18];
	u8 rx_hash_symmetric[0x1];
	u8 reserved_at_101[0x1];
	u8 tunneled_offload_en[0x1];
	u8 reserved_at_103[0x5];
	u8 indirect_table[0x18];
	u8 rx_hash_fn[0x4];
	u8 reserved_at_124[0x2];
	u8 self_lb_block[0x2];
	u8 transport_domain[0x18];
	u8 rx_hash_toeplitz_key[10][0x20];
	struct mlx5_ifc_rx_hash_field_select_bits rx_hash_field_selector_outer;
	struct mlx5_ifc_rx_hash_field_select_bits rx_hash_field_selector_inner;
	u8 reserved_at_2c0[0x4c0];
};

struct mlx5_ifc_create_tir_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 reserved_at_40[0xc0];
	struct mlx5_ifc_tirc_bits ctx;
};

struct mlx5_ifc_rqc_bits {
	u8 rlky[0x1];
	u8 delay_drop_en[0x1];
	u8 scatter_fcs[0x1];
	u8 vsd[0x1];
	u8 mem_rq_type[0x4];
	u8 state[0x4];
	u8 reserved_at_c[0x1];
	u8 flush_in_error_en[0x1];
	u8 hairpin[0x1];
	u8 reserved_at_f[0xB];
	u8 ts_format[0x02];
	u8 reserved_at_1c[0x4];
	u8 reserved_at_20[0x8];
	u8 user_index[0x18];
	u8 reserved_at_40[0x8];
	u8 cqn[0x18];
	u8 counter_set_id[0x8];
	u8 reserved_at_68[0x18];
	u8 reserved_at_80[0x8];
	u8 rmpn[0x18];
	u8 reserved_at_a0[0x8];
	u8 hairpin_peer_sq[0x18];
	u8 reserved_at_c0[0x10];
	u8 hairpin_peer_vhca[0x10];
	u8 reserved_at_e0[0xa0];
	struct mlx5_ifc_wq_bits wq; /* Not used in LRO RQ. */
};

struct mlx5_ifc_create_rq_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 reserved_at_40[0xc0];
	struct mlx5_ifc_rqc_bits ctx;
};

struct mlx5_ifc_cqc_bits {
	u8 status[0x4];
	u8 as_notify[0x1];
	u8 initiator_src_dct[0x1];
	u8 dbr_umem_valid[0x1];
	u8 reserved_at_7[0x1];
	u8 cqe_sz[0x3];
	u8 cc[0x1];
	u8 reserved_at_c[0x1];
	u8 scqe_break_moderation_en[0x1];
	u8 oi[0x1];
	u8 cq_period_mode[0x2];
	u8 cqe_comp_en[0x1];
	u8 mini_cqe_res_format[0x2];
	u8 st[0x4];
	u8 reserved_at_18[0x1];
	u8 cqe_comp_layout[0x7];
	u8 dbr_umem_id[0x20];
	u8 reserved_at_40[0x14];
	u8 page_offset[0x6];
	u8 reserved_at_5a[0x2];
	u8 mini_cqe_res_format_ext[0x2];
	u8 cq_timestamp_format[0x2];
	u8 reserved_at_60[0x3];
	u8 log_cq_size[0x5];
	u8 uar_page[0x18];
	u8 reserved_at_80[0x4];
	u8 cq_period[0xc];
	u8 cq_max_count[0x10];
	u8 reserved_at_a0[0x18];
	u8 c_eqn[0x8];
	u8 reserved_at_c0[0x3];
	u8 log_page_size[0x5];
	u8 reserved_at_c8[0x18];
	u8 reserved_at_e0[0x20];
	u8 reserved_at_100[0x8];
	u8 last_notified_index[0x18];
	u8 reserved_at_120[0x8];
	u8 last_solicit_index[0x18];
	u8 reserved_at_140[0x8];
	u8 consumer_counter[0x18];
	u8 reserved_at_160[0x8];
	u8 producer_counter[0x18];
	u8 local_partition_id[0xc];
	u8 process_id[0x14];
	u8 reserved_at_1A0[0x20];
	u8 dbr_addr[0x40];
};

struct mlx5_ifc_create_cq_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 reserved_at_40[0x40];
	struct mlx5_ifc_cqc_bits cq_context;
	u8 cq_umem_offset[0x40];
	u8 cq_umem_id[0x20];
	u8 cq_umem_valid[0x1];
	u8 reserved_at_2e1[0x1f];
	u8 reserved_at_300[0x580];
	u8 pas[];
};

struct mlx5_ifc_rq_num_bits {
	u8 reserved_at_0[0x8];
	u8 rq_num[0x18];
};

struct mlx5_ifc_rqtc_bits {
	u8 reserved_at_0[0xa5];
	u8 list_q_type[0x3];
	u8 reserved_at_a8[0x8];
	u8 rqt_max_size[0x10];
	u8 reserved_at_c0[0x10];
	u8 rqt_actual_size[0x10];
	u8 reserved_at_e0[0x6a0];
	struct mlx5_ifc_rq_num_bits rq_num[];
};

struct mlx5_ifc_create_rqt_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 reserved_at_40[0xc0];
	struct mlx5_ifc_rqtc_bits rqt_context;
};

struct mlx5_ifc_modify_rq_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];
	u8 syndrome[0x20];
	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_modify_rq_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 rq_state[0x4];
	u8 reserved_at_44[0x4];
	u8 rqn[0x18];
	u8 reserved_at_60[0x20];
	u8 modify_bitmask[0x40];
	u8 reserved_at_c0[0x40];
	struct mlx5_ifc_rqc_bits ctx;
};

struct mlx5_ifc_alloc_transport_domain_in_bits {
	u8 opcode[0x10];
	u8 reserved_at_10[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_create_sq_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 reserved_at_40[0xc0];

	struct mlx5_ifc_sqc_bits ctx;
};

struct mlx5_ifc_create_tis_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 reserved_at_40[0xc0];

	struct mlx5_ifc_tisc_bits ctx;
};

#undef u8

#endif /* FLUX_IOKD_FNET_MLX5_PRM_H */
